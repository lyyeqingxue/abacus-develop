//==========================================================
// AUTHOR : Peize Lin
// DATE :   2017-09-14
// UPDATE : 2021-02-28
//==========================================================

#ifdef USE_LIBXC

#include "potential_libxc.h"
#include "../src_pw/global.h"
#include "../module_base/global_function.h"
#include "../module_base/global_variable.h"
#include "module_base/timer.h"
#include "src_parallel/parallel_reduce.h"
#include "xc_gga_pw.h"
#ifdef __LCAO
#include "../src_lcao/global_fp.h"
#endif

//the interfacd to libxc xc_lda_exc_vxc and xc_gga_exc_vxc
//XC_POLARIZED, XC_UNPOLARIZED: internal flags used in LIBXC, denote the polarized(nspin=1) or unpolarized(nspin=2) calculations, definition can be found in xc.h from LIBXC
//XC_FAMILY_LDA, XC_FAMILY_GGA, XC_FAMILY_HYB_GGA, XC_CORRELATION: internal flags used in LIBXC, denote the types of functional associated with a certain functional ID, definition can be found in xc.h from LIBXC

// [etxc, vtxc, v] = Potential_Libxc::v_xc(...)
std::tuple<double,double,ModuleBase::matrix> Potential_Libxc::v_xc(
	const int &nrxx, // number of real-space grid
	const int &ncxyz, // total number of charge grid
	const double &omega, // volume of cell
	const double * const * const rho_in,
	const double * const rho_core_in)
{
    ModuleBase::TITLE("Potential_Libxc","v_xc");
    ModuleBase::timer::tick("Potential_Libxc","v_xc");

    double etxc = 0.0;
    double vtxc = 0.0;
	ModuleBase::matrix v(GlobalV::NSPIN,nrxx);

	if(GlobalV::VXC_IN_H == 0 )
	{
    	ModuleBase::timer::tick("Potential_Libxc","v_xc");
		return std::make_tuple( etxc, vtxc, std::move(v) );
	}
	//----------------------------------------------------------
	// xc_func_type is defined in Libxc package
	// to understand the usage of xc_func_type,
	// use can check on website, for example:
	// https://www.tddft.org/programs/libxc/manual/libxc-5.1.x/
	//----------------------------------------------------------
	xc_func_type x_func;
	xc_func_type c_func;

	const int xc_polarized = (1==nspin0()) ? XC_UNPOLARIZED : XC_POLARIZED;
	//--------------------------------------
	// determine exchange functional
	//--------------------------------------
	if( 6==GlobalC::xcf.iexch_now && 
		8==GlobalC::xcf.igcx_now && 
		4==GlobalC::xcf.icorr_now && 
		4==GlobalC::xcf.igcc_now ) // GGA functional
	{
		xc_func_init( &x_func, XC_HYB_GGA_XC_PBEH, xc_polarized );
		double parameter_hse[3] = { GlobalC::exx_global.info.hybrid_alpha, 
			GlobalC::exx_global.info.hse_omega, 
			GlobalC::exx_global.info.hse_omega };
		xc_func_set_ext_params( &x_func, parameter_hse);
	}
	else if( 9==GlobalC::xcf.iexch_now && 
			12==GlobalC::xcf.igcx_now && 
			4==GlobalC::xcf.icorr_now && 
			4==GlobalC::xcf.igcc_now ) // HSE06 hybrid functional
	{
		xc_func_init( &x_func, XC_HYB_GGA_XC_HSE06, xc_polarized );	
		double parameter_hse[3] = { GlobalC::exx_global.info.hybrid_alpha, 
			GlobalC::exx_global.info.hse_omega, 
			GlobalC::exx_global.info.hse_omega };
		xc_func_set_ext_params(&x_func, parameter_hse);
	}
	
	if( 1==GlobalC::xcf.iexch_now && 
		0==GlobalC::xcf.igcx_now ) // LDA functional
	{
		xc_func_init( &x_func, XC_LDA_X, xc_polarized );
	}
	else if( 1==GlobalC::xcf.iexch_now && 
		3==GlobalC::xcf.igcx_now ) // GGA functional
	{
		xc_func_init( &x_func, XC_GGA_X_PBE, xc_polarized );
	}
	else if(GlobalC::xcf.igcx_now == 13 ) //SCAN_X
	{
		xc_func_init( &x_func, 263, xc_polarized );
	}
	else
	{
		throw std::domain_error("iexch="+ModuleBase::GlobalFunc::TO_STRING(GlobalC::xcf.iexch_now)+", igcx="+ModuleBase::GlobalFunc::TO_STRING(GlobalC::xcf.igcx_now)
			+" unfinished in "+ModuleBase::GlobalFunc::TO_STRING(__FILE__)+" line "+ModuleBase::GlobalFunc::TO_STRING(__LINE__));
	}

	//--------------------------------------
	// determine correlation functional
	//--------------------------------------
	if( 1==GlobalC::xcf.icorr_now && 0==GlobalC::xcf.igcc_now )
	{
		xc_func_init( &c_func, XC_LDA_C_PZ, xc_polarized );
	}
	else if( 4==GlobalC::xcf.icorr_now && 4==GlobalC::xcf.igcc_now )
	{
		xc_func_init( &c_func, XC_GGA_C_PBE, xc_polarized );
	}
	else if (GlobalC::xcf.igcc_now == 9)
	{
		xc_func_init( &c_func, 267, xc_polarized );
	}
	else
	{
		throw std::domain_error("icorr="+ModuleBase::GlobalFunc::TO_STRING(GlobalC::xcf.icorr_now)+", igcc="+ModuleBase::GlobalFunc::TO_STRING(GlobalC::xcf.igcc_now)
			+" unfinished in "+ModuleBase::GlobalFunc::TO_STRING(__FILE__)+" line "+ModuleBase::GlobalFunc::TO_STRING(__LINE__));
	}

	bool is_gga = false;
	switch( x_func.info->family )
	{
		case XC_FAMILY_GGA:
		case XC_FAMILY_HYB_GGA:
			is_gga = true;
			break;
	}
	switch( c_func.info->family )
	{
		case XC_FAMILY_GGA:
		case XC_FAMILY_HYB_GGA:
			is_gga = true;
			break;
	}

	// converting rho
	std::vector<double> rho;	
	rho.resize(GlobalC::pw.nrxx*nspin0());
	if(nspin0()==1 || GlobalV::NSPIN==2)
	{
		for( int is=0; is!=nspin0(); ++is )
			for( int ir=0; ir!=GlobalC::pw.nrxx; ++ir )
				rho[ir*nspin0()+is] = rho_in[is][ir] + 1.0/nspin0()*rho_core_in[ir];
	}
	else // may need updates for SOC
	{
		if(GlobalC::xcf.igcx||GlobalC::xcf.igcc)
		{
			GlobalC::ucell.cal_ux();
		}
		for( int ir=0; ir!=GlobalC::pw.nrxx; ++ir )
		{
			const double amag = sqrt( pow(rho_in[1][ir],2) + pow(rho_in[2][ir],2) + pow(rho_in[3][ir],2) );
			const double neg = (GlobalC::ucell.magnet.lsign_ && rho_in[1][ir]*GlobalC::ucell.magnet.ux_[0]
								+rho_in[2][ir]*GlobalC::ucell.magnet.ux_[1]
								+rho_in[3][ir]*GlobalC::ucell.magnet.ux_[2]<=0)
								? -1 : 1;
			rho[ir*2]   = 0.5 * (rho_in[0][ir] + neg * amag) + 0.5 * rho_core_in[ir];
			rho[ir*2+1] = 0.5 * (rho_in[0][ir] - neg * amag) + 0.5 * rho_core_in[ir];
		}
	}

	std::vector<std::vector<ModuleBase::Vector3<double>>> gdr;
	std::vector<double> sigma;
	if(is_gga)
	{
		// calculating grho
		gdr.resize( nspin0() );
		for( int is=0; is!=nspin0(); ++is )
		{
			std::vector<double> rhor(GlobalC::pw.nrxx);
			for(int ir=0; ir<GlobalC::pw.nrxx; ++ir)
				rhor[ir] = rho[ir*nspin0()+is];

			//------------------------------------------
			// initialize the charge density arry in reciprocal space
			// bring electron charge density from real space to reciprocal space
			//------------------------------------------
			std::vector<std::complex<double>> rhog(GlobalC::pw.ngmc);
			GlobalC::CHR.set_rhog(rhor.data(), rhog.data());

			//-------------------------------------------
			// compute the gradient of charge density and
			// store the gradient in gdr[is]
			//-------------------------------------------
			gdr[is].resize(GlobalC::pw.nrxx);
			GGA_PW::grad_rho(rhog.data(), gdr[is].data());
		}

		// converting grho
		sigma.resize( nrxx * ((1==nspin0())?1:3) );

		if( 1==nspin0() )
		{
			for( int ir=0; ir!=GlobalC::pw.nrxx; ++ir )
			{
				sigma[ir] = gdr[0][ir]*gdr[0][ir];
			}
		}
		else
		{
			for( int ir=0; ir!=GlobalC::pw.nrxx; ++ir )
			{
				sigma[ir*3]   = gdr[0][ir]*gdr[0][ir];
				sigma[ir*3+1] = gdr[0][ir]*gdr[1][ir];
				sigma[ir*3+2] = gdr[1][ir]*gdr[1][ir];
			}
		}
	}

	// jiyy add for threshold
	constexpr double rho_threshold = 1E-10;
	xc_func_set_dens_threshold(&x_func, rho_threshold);
	// sgn for threshold mask
	std::vector<double> sgn( nrxx * nspin0(), 1.0);

	for( int ir=0; ir!= nrxx * nspin0(); ++ir )
	{
		if ( rho[ir]<rho_threshold)  sgn[ir] = 0.0;
	}

	std::vector<double> exc   ( nrxx                       );
	std::vector<double> vrho  ( nrxx * nspin0()            );
	std::vector<double> vsigma( nrxx * ((1==nspin0())?1:3) );

	switch( x_func.info->family )
	{
		case XC_FAMILY_LDA:
			// call Libxc function: xc_lda_exc_vxc
			xc_lda_exc_vxc( &x_func, nrxx, rho.data(), 
				exc.data(), vrho.data() );
			break;
		case XC_FAMILY_GGA:
		case XC_FAMILY_HYB_GGA:
			// call Libxc function: xc_gga_exc_vxc
			xc_gga_exc_vxc( &x_func, nrxx, rho.data(), sigma.data(),
				exc.data(), vrho.data(), vsigma.data() );
			break;
		default:
			throw std::domain_error("func.info->family ="+ModuleBase::GlobalFunc::TO_STRING(x_func.info->family)
				+" unfinished in "+ModuleBase::GlobalFunc::TO_STRING(__FILE__)+" line "+ModuleBase::GlobalFunc::TO_STRING(__LINE__));
			break;
	}
	for( int is=0; is!=nspin0(); ++is )
	{
		for( int ir=0; ir!= nrxx; ++ir )
		{
			etxc += ModuleBase::e2 * exc[ir] * rho[ir*nspin0()+is] * sgn[ir*nspin0()+is];
		}
	}

	if(nspin0()==1 || GlobalV::NSPIN==2)
	{
		for( int is=0; is!=nspin0(); ++is )
		{
			for( int ir=0; ir!= nrxx; ++ir )
			{
				const double v_tmp = ModuleBase::e2 * vrho[ir*nspin0()+is] * sgn[ir*nspin0()+is];
				v(is,ir) += v_tmp;
				vtxc += v_tmp * rho_in[is][ir];
			}
		}
	}
	else // may need updates for SOC
	{
		constexpr double vanishing_charge = 1.0e-12;
		for( int ir=0; ir!= nrxx; ++ir )
		{
			std::vector<double> v_tmp(4);
			v_tmp[0] = ModuleBase::e2 * (0.5 * (vrho[ir*2] + vrho[ir*2+1]));
			const double vs = 0.5 * (vrho[ir*2] - vrho[ir*2+1]);
			const double amag = sqrt( pow(rho_in[1][ir],2) 
				+ pow(rho_in[2][ir],2) 
				+ pow(rho_in[3][ir],2) );

			if(amag>vanishing_charge)
			{
				for(int ipol=1; ipol<4; ++ipol)
				{
					v_tmp[ipol] = ModuleBase::e2 * vs * rho_in[ipol][ir] / amag;
				}
			}
			for(int ipol=0; ipol<4; ++ipol)
			{
				v(ipol, ir) += v_tmp[ipol];
				vtxc += v_tmp[ipol] * rho_in[ipol][ir];
			}
		}
	}

	if(x_func.info->family == XC_FAMILY_GGA || x_func.info->family == XC_FAMILY_HYB_GGA)
	{	
		std::vector<std::vector<ModuleBase::Vector3<double>>> h( nspin0(), std::vector<ModuleBase::Vector3<double>>(nrxx) );
		if( 1==nspin0() )
		{
			for( int ir=0; ir!= nrxx; ++ir )
			{
				h[0][ir] = ModuleBase::e2 * gdr[0][ir] * vsigma[ir] * 2.0 * sgn[ir];
			}
		}
		else
		{
			for( int ir=0; ir!= nrxx; ++ir )
			{
				h[0][ir] = ModuleBase::e2 * (gdr[0][ir] * vsigma[ir*3  ] * 2.0 * sgn[ir*2  ]
								+ gdr[1][ir] * vsigma[ir*3+1]       * sgn[ir*2]   * sgn[ir*2+1]);
				h[1][ir] = ModuleBase::e2 * (gdr[1][ir] * vsigma[ir*3+2] * 2.0 * sgn[ir*2+1]
								+ gdr[0][ir] * vsigma[ir*3+1]       * sgn[ir*2]   * sgn[ir*2+1]);
			}
		}

		// define two dimensional array dh [ nspin, GlobalC::pw.nrxx ]
		std::vector<std::vector<double>> dh(nspin0(), std::vector<double>( nrxx));
		for( int is=0; is!=nspin0(); ++is )
			GGA_PW::grad_dot( ModuleBase::GlobalFunc::VECTOR_TO_PTR(h[is]), ModuleBase::GlobalFunc::VECTOR_TO_PTR(dh[is]) );

		for( int is=0; is!=nspin0(); ++is )
			for( int ir=0; ir!= nrxx; ++ir )
				vtxc -= dh[is][ir] * rho[ir*nspin0()+is];

		if(nspin0()==1 || GlobalV::NSPIN==2)
		{
			for( int is=0; is!=nspin0(); ++is )
				for( int ir=0; ir!= nrxx; ++ir )
					v(is,ir) -= dh[is][ir];
		}
		else // may need updates for SOC
		{
			constexpr double vanishing_charge = 1.0e-12;
			for( int ir=0; ir!= nrxx; ++ir )
			{
				v(0,ir) -= 0.5 * (dh[0][ir] + dh[1][ir]);
				const double amag = sqrt( pow(rho_in[1][ir],2) + pow(rho_in[2][ir],2) + pow(rho_in[3][ir],2) );
				const double neg = (GlobalC::ucell.magnet.lsign_
									&& rho_in[1][ir]*GlobalC::ucell.magnet.ux_[0]+rho_in[2][ir]*GlobalC::ucell.magnet.ux_[1]+rho_in[3][ir]*GlobalC::ucell.magnet.ux_[2]<=0)
									? -1 : 1;
				if(amag > vanishing_charge)
				{
					for(int i=1;i<4;i++)
						v(i,ir) -= neg * 0.5 * (dh[0][ir]-dh[1][ir]) * rho_in[i][ir] / amag;
				}
			}
		}
	}

	switch( c_func.info->family )
	{
		case XC_FAMILY_LDA:
			// call Libxc function: xc_lda_exc_vxc
			xc_lda_exc_vxc( &c_func, nrxx, rho.data(), 
				exc.data(), vrho.data() );
			break;
		case XC_FAMILY_GGA:
		case XC_FAMILY_HYB_GGA:
			// call Libxc function: xc_gga_exc_vxc
			xc_gga_exc_vxc( &c_func, nrxx, rho.data(), sigma.data(),
				exc.data(), vrho.data(), vsigma.data() );
			break;
		default:
			throw std::domain_error("func.info->family ="+ModuleBase::GlobalFunc::TO_STRING(c_func.info->family)
				+" unfinished in "+ModuleBase::GlobalFunc::TO_STRING(__FILE__)+" line "+ModuleBase::GlobalFunc::TO_STRING(__LINE__));
			break;
	}
	for( int is=0; is!=nspin0(); ++is )
	{
		for( int ir=0; ir!= nrxx; ++ir )
		{
			etxc += ModuleBase::e2 * exc[ir] * rho[ir*nspin0()+is] * sgn[ir*nspin0()+is];
		}
	}

	if(nspin0()==1 || GlobalV::NSPIN==2)
	{
		for( int is=0; is!=nspin0(); ++is )
		{
			for( int ir=0; ir!= nrxx; ++ir )
			{
				const double v_tmp = ModuleBase::e2 * vrho[ir*nspin0()+is] * sgn[ir*nspin0()+is];
				v(is,ir) += v_tmp;
				vtxc += v_tmp * rho_in[is][ir];
			}
		}
	}
	else // may need updates for SOC
	{
		constexpr double vanishing_charge = 1.0e-12;
		for( int ir=0; ir!= nrxx; ++ir )
		{
			std::vector<double> v_tmp(4);
			v_tmp[0] = ModuleBase::e2 * (0.5 * (vrho[ir*2] + vrho[ir*2+1]));
			const double vs = 0.5 * (vrho[ir*2] - vrho[ir*2+1]);
			const double amag = sqrt( pow(rho_in[1][ir],2) 
				+ pow(rho_in[2][ir],2) 
				+ pow(rho_in[3][ir],2) );

			if(amag>vanishing_charge)
			{
				for(int ipol=1; ipol<4; ++ipol)
				{
					v_tmp[ipol] = ModuleBase::e2 * vs * rho_in[ipol][ir] / amag;
				}
			}
			for(int ipol=0; ipol<4; ++ipol)
			{
				v(ipol, ir) += v_tmp[ipol];
				vtxc += v_tmp[ipol] * rho_in[ipol][ir];
			}
		}
	}

	if(c_func.info->family == XC_FAMILY_GGA || c_func.info->family == XC_FAMILY_HYB_GGA)
	{	
		std::vector<std::vector<ModuleBase::Vector3<double>>> h( nspin0(), std::vector<ModuleBase::Vector3<double>>(nrxx) );
		if( 1==nspin0() )
		{
			for( int ir=0; ir!= nrxx; ++ir )
			{
				h[0][ir] = ModuleBase::e2 * gdr[0][ir] * vsigma[ir] * 2.0 * sgn[ir];
			}
		}
		else
		{
			for( int ir=0; ir!= nrxx; ++ir )
			{
				h[0][ir] = ModuleBase::e2 * (gdr[0][ir] * vsigma[ir*3  ] * 2.0 * sgn[ir*2  ]
								+ gdr[1][ir] * vsigma[ir*3+1]       * sgn[ir*2]   * sgn[ir*2+1]);
				h[1][ir] = ModuleBase::e2 * (gdr[1][ir] * vsigma[ir*3+2] * 2.0 * sgn[ir*2+1]
								+ gdr[0][ir] * vsigma[ir*3+1]       * sgn[ir*2]   * sgn[ir*2+1]);
			}
		}

		// define two dimensional array dh [ nspin, GlobalC::pw.nrxx ]
		std::vector<std::vector<double>> dh(nspin0(), std::vector<double>( nrxx));
		for( int is=0; is!=nspin0(); ++is )
			GGA_PW::grad_dot( ModuleBase::GlobalFunc::VECTOR_TO_PTR(h[is]), ModuleBase::GlobalFunc::VECTOR_TO_PTR(dh[is]) );

		for( int is=0; is!=nspin0(); ++is )
			for( int ir=0; ir!= nrxx; ++ir )
				vtxc -= dh[is][ir] * rho[ir*nspin0()+is];

		if(nspin0()==1 || GlobalV::NSPIN==2)
		{
			for( int is=0; is!=nspin0(); ++is )
				for( int ir=0; ir!= nrxx; ++ir )
					v(is,ir) -= dh[is][ir];
		}
		else // may need updates for SOC
		{
			constexpr double vanishing_charge = 1.0e-12;
			for( int ir=0; ir!= nrxx; ++ir )
			{
				v(0,ir) -= 0.5 * (dh[0][ir] + dh[1][ir]);
				const double amag = sqrt( pow(rho_in[1][ir],2) + pow(rho_in[2][ir],2) + pow(rho_in[3][ir],2) );
				const double neg = (GlobalC::ucell.magnet.lsign_
									&& rho_in[1][ir]*GlobalC::ucell.magnet.ux_[0]+rho_in[2][ir]*GlobalC::ucell.magnet.ux_[1]+rho_in[3][ir]*GlobalC::ucell.magnet.ux_[2]<=0)
									? -1 : 1;
				if(amag > vanishing_charge)
				{
					for(int i=1;i<4;i++)
						v(i,ir) -= neg * 0.5 * (dh[0][ir]-dh[1][ir]) * rho_in[i][ir] / amag;
				}
			}
		}
	}

	//-------------------------------------------------
	// for MPI, reduce the exchange-correlation energy
	//-------------------------------------------------
	Parallel_Reduce::reduce_double_pool( etxc );
	Parallel_Reduce::reduce_double_pool( vtxc );

    etxc *= omega / ncxyz;
    vtxc *= omega / ncxyz;

    ModuleBase::timer::tick("Potential_Libxc","v_xc");
	return std::make_tuple( etxc, vtxc, std::move(v) );
}

#endif	//ifdef USE_LIBXC
