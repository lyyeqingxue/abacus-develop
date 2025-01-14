#include "paw_cell.h"
#include "module_base/tool_title.h"
#include "module_base/tool_quit.h"

void Paw_Cell::init_paw_cell(
    const double ecutwfc_in, const double cell_factor_in,
    const int nat_in, const int ntyp_in,
    const int * atom_type_in, const double ** atom_coord_in,
    const std::vector<std::string> & filename_list_in,
    const int nx_in, const int ny_in, const int nz_in,
    const std::complex<double> * eigts1_in, const std::complex<double> * eigts2_in, const std::complex<double> * eigts3_in)
{
    ModuleBase::TITLE("Paw_Element","init_paw_cell");

    this -> nat = nat_in;
    this -> ntyp = ntyp_in;
    this -> nx = nx_in;
    this -> ny = ny_in;
    this -> nz = nz_in;

    atom_coord.resize(nat);
    atom_type.resize(nat);
    for(int iat = 0; iat < nat; iat ++)
    {
        atom_coord[iat].resize(3);
        for(int i = 0; i < 3; i ++)
        {
            atom_coord[iat][i] = atom_coord_in[iat][i];
        }

        atom_type[iat] = atom_type_in[iat];
    }

    paw_element_list.resize(ntyp);
    assert(filename_list_in.size() == ntyp);
    for(int ityp = 0; ityp < ntyp; ityp ++)
    {
        paw_element_list[ityp].init_paw_element(ecutwfc_in, cell_factor_in);
        paw_element_list[ityp].read_paw_xml(filename_list_in[ityp]);
    }

    this->map_paw_proj();

    eigts1.resize(nat);
    eigts2.resize(nat);
    eigts3.resize(nat);

    for(int iat = 0; iat < nat; iat ++)
    {
        eigts1[iat].resize(2*nx+1);
        eigts2[iat].resize(2*ny+1);
        eigts3[iat].resize(2*nz+1);

        for(int i = 0; i < 2*nx+1; i ++)
        {
            eigts1[iat][i] = eigts1_in[ iat * (2*nx+1) + i];
        }

        for(int i = 0; i < 2*ny+1; i ++)
        {
            eigts2[iat][i] = eigts2_in[ iat * (2*ny+1) + i];
        }

        for(int i = 0; i < 2*nz+1; i ++)
        {
            eigts3[iat][i] = eigts3_in[ iat * (2*nz+1) + i];
        }
    }
}

// exp(-i(k+G)R_I) = exp(-ikR_I) exp(-iG_xR_Ix) exp(-iG_yR_Iy) exp(-iG_zR_Iz)
void Paw_Cell::set_paw_k(
    const int npw, const double * kpt,
    const int * ig_to_ix, const int * ig_to_iy, const int * ig_to_iz,
    const double ** kpg)
{
    ModuleBase::TITLE("Paw_Element","set_paw_k");

    const double pi = 3.141592653589793238462643383279502884197;
    const double twopi = 2.0 * pi;

    struc_fact.resize(nat);
    for(int iat = 0; iat < nat; iat ++)
    {
        double arg = 0.0;
        for(int i = 0; i < 3; i ++)
        {
            arg += atom_coord[iat][i] * kpt[i];
        }
        arg *= twopi;
        const std::complex<double> kphase = std::complex<double>(cos(arg), -sin(arg));

        struc_fact[iat].resize(npw);

        for(int ipw = 0; ipw < npw; ipw ++)
        {
            int ix = ig_to_ix[ipw];
            int iy = ig_to_iy[ipw];
            int iz = ig_to_iz[ipw];

            struc_fact[iat][ipw] = kphase * eigts1[iat][ix] * eigts2[iat][iy] * eigts3[iat][iz];
        }
    }

    this -> set_ylm(npw, kpg);
}

void Paw_Cell::map_paw_proj()
{
    ModuleBase::TITLE("Paw_Element","map_paw_proj");

    nproj_tot = 0;
    
    for(int ia = 0; ia < nat; ia ++)
    {
        int it = atom_type[ia];
        nproj_tot += paw_element_list[it].get_mstates();
    }

    // Not sure if all of them will be used, but it doesn't
    // hurt to prepare them I suppose
    // Doesn't take much time, also not much space

    iprj_to_ia.resize(nproj_tot);
    iprj_to_im.resize(nproj_tot);
    iprj_to_il.resize(nproj_tot);
    iprj_to_l.resize(nproj_tot);
    iprj_to_m.resize(nproj_tot);

    int iproj = 0;
    for(int ia = 0; ia < nat; ia ++)
    {
        int it = atom_type[ia];
        int mstates = paw_element_list[it].get_mstates();
        std::vector<int> im_to_istate = paw_element_list[it].get_im_to_istate();
        std::vector<int> lstate = paw_element_list[it].get_lstate();
        std::vector<int> mstate = paw_element_list[it].get_mstate();

        for(int im = 0; im < mstates; im ++)
        {
            iprj_to_ia[iproj] = ia;
            iprj_to_im[iproj] = im;
            iprj_to_il[iproj] = im_to_istate[im];
            iprj_to_l [iproj] = lstate[im_to_istate[im]];
            iprj_to_m [iproj] = mstate[im];
            iproj ++;
        }
    }

    lmax = 0;
    for(int it = 0; it < ntyp; it ++)
    {
        lmax = std::max(lmax, paw_element_list[it].get_lmax());
    }

    assert(iproj == nproj_tot);
}

void Paw_Cell::set_ylm(const int npw, const double ** kpg)
{
    ylm_k.resize(npw);
    for(int ipw = 0; ipw < npw; ipw ++)
    {
        ylm_k[ipw] = calc_ylm(lmax, kpg[ipw]);
    }
}

std::vector<double> Paw_Cell::calc_ylm(const int lmax, const double * r)
{
    // there are altogether (l+1)^2 spherical harmonics
    // up to angular momentum l
    int size_ylm = (lmax + 1) * (lmax + 1);

    std::vector<double> ylm;
    ylm.resize(size_ylm);

    double xx = r[0];
    double yy = r[1];
    double zz = r[2];
    double rr = std::sqrt(xx*xx+yy*yy+zz*zz);

    double tol = 1e-10;

    double cphi, sphi, ctheta, stheta;
    std::vector<std::complex<double>> phase;
    phase.resize(lmax+1);

    const double pi = 3.141592653589793238462643383279502884197;
    const double fourpi = 4.0 * pi;

    // set zero
    for(int i = 0; i < size_ylm; i++)
    {
        ylm[i] = 0;
    }

    // l = 0
    ylm[0] = 1.0/std::sqrt(fourpi);

    if(rr>tol)
    {
        cphi = 1.0;
        sphi = 0.0;
        ctheta = zz/rr;
        stheta = std::sqrt(std::abs((1.0-ctheta)*(1.0+ctheta)));

        if(stheta > tol)
        {
            cphi = xx/(rr*stheta); // cos (phi)
            sphi = yy/(rr*stheta); // sin (phi)
        }
        
        for(int m = 1; m < lmax + 1; m ++)
        {
            std::complex<double> tmp(cphi,sphi);
            phase[m] = std::pow(tmp,m); //exp(i m phi)
        }

        for(int l = 1; l < lmax + 1; l++)
        {
            const int l0 = l*l + l;
            double fact  = 1.0/double(l*(l+1));
            const double ylmcst = std::sqrt(double(2*l+1)/fourpi);
            
            // m = 0
            ylm[l0] = ylmcst*Paw_Cell::ass_leg_pol(l,0,ctheta);
            
            // m > 0
            double onem = 1.0;
            for(int m = 1; m < l + 1; m ++)
            {
                onem = - onem; // the factor (-1)^m
                double work1 = ylmcst*std::sqrt(fact)*onem
                    *Paw_Cell::ass_leg_pol(l,m,ctheta)*std::sqrt(2.0);
                ylm[l0+m] = work1 * phase[m].real();
                ylm[l0-m] = work1 * phase[m].imag();
                if(m != l) fact = fact/double((l+m+1)*(l-m));
            }
        }
    }

    return ylm;

}

double Paw_Cell::ass_leg_pol(const int l, const int m, const double arg)
{

    double x = arg;
    if(m < 0 || m > l || std::abs(x) > 1.0)
    {
        if(std::abs(x) > 1.0 + 1e-10)
        {
            ModuleBase::WARNING_QUIT("Paw_Cell","bad argument l, m, or x");
        }
        x = 1.0;
    }

    double polmm = 1.0;
    if(m > 0)
    {
        double sqrx = std::sqrt(std::abs((1.0-x) * (1.0+x)));
        for(int i = 0; i < m; i++)
        {
            polmm = polmm * (1.0 - 2.0 * double(i+1)) * sqrx;
        }
    }

    if(l == m)
    {
        return polmm;
    }
    else
    {
        double tmp1 = x * (2.0 * double(m) + 1.0) * polmm;
        if(l == m+1)
        {
            return tmp1;
        }
        else
        {
            double pll;
            for(int ll = m+2; ll < l+1; ll ++)
            {
                pll = (x*(2.0*ll-1.0)*tmp1-(ll+m-1.0)*polmm)/double(ll-m);
                polmm = tmp1;
                tmp1 = pll;
            }
            return pll;
        }
    }
}