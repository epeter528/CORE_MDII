/* -*- mode: c; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; c-file-style: "stroustrup"; -*-
 *
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Gallium Rubidium Oxygen Manganese Argon Carbon Silicon
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if ((defined WIN32 || defined _WIN32 || defined WIN64 || defined _WIN64) && !defined __CYGWIN__ && !defined __CYGWIN32__)
/* _isnan() */
#include <float.h>
#endif

#include "typedefs.h"
#include "smalloc.h"
#include "sysstuff.h"
#include "vec.h"
#include "statutil.h"
#include "vcm.h"
#include "mdebin.h"
#include "nrnb.h"
#include "calcmu.h"
#include "index.h"
#include "vsite.h"
#include "update.h"
#include "ns.h"
#include "trnio.h"
#include "xtcio.h"
#include "mdrun.h"
#include "confio.h"
#include "network.h"
#include "pull.h"
#include "xvgr.h"
#include "physics.h"
#include "names.h"
#include "xmdrun.h"
#include "ionize.h"
#include "disre.h"
#include "orires.h"
#include "dihre.h"
#include "pppm.h"
#include "pme.h"
#include "mdatoms.h"
#include "repl_ex.h"
#include "qmmm.h"
#include "mpelogging.h"
#include "domdec.h"
#include "partdec.h"
#include "topsort.h"
#include "coulomb.h"
#include "constr.h"
#include "shellfc.h"
#include "compute_io.h"
#include "mvdata.h"
#include "checkpoint.h"
#include "mtop_util.h"
#include "sighandler.h"
#include "string2.h"
#include <sys/stat.h>

#ifdef GMX_LIB_MPI
#include <mpi.h>
#endif
#ifdef GMX_THREADS
#include "tmpi.h"
#endif

#ifdef GMX_FAHCORE
#include "corewrap.h"
#endif


double do_md(FILE *fplog,t_commrec *cr,int nfile,const t_filenm fnm[],
             const output_env_t oenv, gmx_bool bVerbose,gmx_bool bCompact,
             int nstglobalcomm,
             gmx_vsite_t *vsite,gmx_constr_t constr,
             int stepout,t_inputrec *ir,
             gmx_mtop_t *top_global,
             t_fcdata *fcd,
             t_state *state_global,
             t_mdatoms *mdatoms,
             t_nrnb *nrnb,gmx_wallcycle_t wcycle,
             gmx_edsam_t ed,t_forcerec *fr,
             int repl_ex_nst,int repl_ex_seed,
             real cpt_period,real max_hours,
             const char *deviceOptions,
             unsigned long Flags,
             gmx_runtime_t *runtime)
{
    gmx_mdoutf_t *outf;
    gmx_large_int_t step,step_rel;
    double     run_time;
    double     t,t0,lam0;
    gmx_bool       bGStatEveryStep,bGStat,bNstEner,bCalcEnerPres;
    gmx_bool       bNS,bNStList,bSimAnn,bStopCM,bRerunMD,bNotLastFrame=FALSE,
               bFirstStep,bStateFromTPX,bInitStep,bLastStep,
               bBornRadii,bStartingFromCpt;
    gmx_bool       bDoDHDL=FALSE;
    gmx_bool       do_ene,do_log,do_verbose,bRerunWarnNoV=TRUE,
               bForceUpdate=FALSE,bCPT;
    int        mdof_flags;
    gmx_bool       bMasterState;
    int        force_flags,cglo_flags;
    tensor     force_vir,shake_vir,total_vir,tmp_vir,pres;
    int        i,m;
    t_trxstatus *status;
    rvec       mu_tot;
    t_vcm      *vcm;
    t_state    *bufstate=NULL;   
    matrix     *scale_tot,pcoupl_mu,M,ebox;
    gmx_nlheur_t nlh;
    t_trxframe rerun_fr;
    gmx_repl_ex_t repl_ex=NULL;
    int        nchkpt=1;

    gmx_localtop_t *top;	
    t_mdebin *mdebin=NULL;
    t_state    *state=NULL;
    rvec       *f_global=NULL;
    int        n_xtc=-1;
    rvec       *x_xtc=NULL;
    gmx_enerdata_t *enerd;
    rvec       *f=NULL;
    gmx_global_stat_t gstat;
    gmx_update_t upd=NULL;
    t_graph    *graph=NULL;
    globsig_t   gs;

    gmx_bool        bFFscan;
    gmx_groups_t *groups;
    gmx_ekindata_t *ekind, *ekind_save;
    gmx_shellfc_t shellfc;
    int         count,nconverged=0;
    real        timestep=0;
    double      tcount=0;
    gmx_bool        bIonize=FALSE;
    gmx_bool        bTCR=FALSE,bConverged=TRUE,bOK,bSumEkinhOld,bExchanged;
    gmx_bool        bAppend;
    gmx_bool        bResetCountersHalfMaxH=FALSE;
    gmx_bool        bVV,bIterations,bFirstIterate,bTemp,bPres,bTrotter;
    real        temp0,mu_aver=0,dvdl;
    int         a0,a1,gnx=0,ii;
    atom_id     *grpindex=NULL;
    char        *grpname;
    t_coupl_rec *tcr=NULL;
    rvec        *xcopy=NULL,*vcopy=NULL,*cbuf=NULL;
    matrix      boxcopy={{0}},lastbox;
	tensor      tmpvir;
	real        fom,oldfom,veta_save,pcurr,scalevir,tracevir;
	real        vetanew = 0;
    double      cycles;
	real        saved_conserved_quantity = 0;
    real        last_ekin = 0;
	int         iter_i;
	t_extmass   MassQ;
// new vars

    double **diff_prob_x,**diff_prob_y,**diff_prob_z;
    double force_level;
    double max_prob;
    double min_prob;
    double f_diff_x,f_diff_y,f_diff_z;
    double *grad_x,*grad_y,*grad_z;    
    double *grad_x2,*grad_y2,*grad_z2;
    double *ener_g;
    double diff_x,diff_y,diff_z;
    double d_tot,ran=0.0,ran2=0.0,ran3=0.0;
    double time = 0.0;
    double maxf,maxf0,gamma,maxf1=0.0;
    double boost=1.0;
   int dstart,dend;
   int *count_0;
   double box_length;
   int stepper=0;    
   int k;
   double *norm,norm1,norm2;	
    int         **trotter_seq; 
    char        sbuf[STEPSTRSIZE],sbuf2[STEPSTRSIZE];
    int         handled_stop_condition=gmx_stop_cond_none; /* compare to get_stop_condition*/
    gmx_iterate_t iterate;
    gmx_large_int_t multisim_nsteps=-1; /* number of steps to do  before first multisim 
                                          simulation stops. If equal to zero, don't
                                          communicate any more between multisims.*/
#ifdef GMX_FAHCORE
    /* Temporary addition for FAHCORE checkpointing */
    int chkpt_ret;
#endif

    double r_g,r_g_tot,d_range,d_t,d_t2,d_t3;
    double av_x,av_y,av_z,rg=0.0;
    int p=0,step_father[51],l,r,l1,l2,add_int;
    int natoms1,n,modpar,number_of_centers;
    int *at_id,min_range_center;
    double df_x_tot,df_y_tot,df_z_tot;
    double ax_tot, ay_tot, az_tot;
    double d_tot2;
    double *dd_fx,*dd_fy,*dd_fz;    
    double PI,min_fluct_par;
    double wt_meta_delta_t=0.0,wt_meta_delta_t2=0.0,accept=0.0,spec_dist=0.0;
    int    k_max;

    double width_WA=0.0,s_factor=0.0; 
    int number_of_replicas;
    double *dx_3,*dy_3,*dz_3;
    double delta_s2=0.0;
    double *d_t_strich;    
    double *dL_x,*dL_y,*dL_z;
    double *pot2,*pot; 
    double h3,h4,h5;
    double epsilon1;
    double diff_state_x, diff_state_y, diff_state_z;
    double sum_exp_s_x, sum_exp_s_y, sum_exp_s_z;
    double state_x, state_y, state_z;    
    double p_sol;
    int *restraint_sampling;
    int *at_id_r,*at_id_r2;
    double *distance_restraint;
    int number_of_restraints = 0;  
    double cos_val,dot_prod = 0.0;
    double d_x,d_y,d_z,d_x2,d_y2,d_z2;    
    struct stat *buf = malloc(sizeof(struct stat));
    double alpha_sum_x = 0.0, alpha_sum_y=0.0, alpha_sum_z=0.0;   
    double a_param=0.0, gamma_param=0.0, sigma_param=0.0;
    double diff_f_x,diff_f_y,diff_f_z;
    double state_last_x,state_last_y,state_last_z;  
    double diff_vec_x, diff_vec_y, diff_vec_z;
    double diff_vel_x, diff_vel_y, diff_vel_z;
    double dot_prod_norm;
    double com_x,com_y,com_z;
    double comf_x,comf_y,comf_z;  
    double prob_sol;
    double r_cutoff=1.4,alpha=1.0;
    int number_of_water_sites;    

    double *probability_x1,*probability_y1,*probability_z1;
    double Z_x2,Z_y2,Z_z2;
    double dv_x1,dv_y1,dv_z1;
    double df_x1,df_y1,df_z1;
    double *x_1,*y_1,*z_1;
    double *diff_s_x,*diff_s_y,*diff_s_z,h2;
    double *diff_s_xc,*diff_s_yc,*diff_s_zc;
    double *diff_s_x1,*diff_s_y1,*diff_s_z1;   
    double *diff_s_x2,*diff_s_y2,*diff_s_z2;   
    double *diff_L_xc,*diff_L_yc,*diff_L_zc;    
    
    double *dLL_x,*dLL_y,*dLL_z,*time_step;
    double *d_xx2,*d_yy2,*d_zz2;    
    double *sum_state_x,*sum_state_y,*sum_state_z;
    double number_k=0.0,*stepper_d,l_mean=0.0,red_temp;
    double abs_vec=0.0,exp_vec=0.0,*tau_1; 
    double diff_Z_x,diff_Z_y,diff_Z_z;
    int    *meas_x,*meas_y,*meas_z;
    int    *meas_x_last,*meas_y_last,*meas_z_last;
    int    mod_stepper=0;
    int    *flag_update;
    int    flag_run_input = 0;
    double reg_const=1E-2;
    double W_factor = 1.0;
    int    update_int = 0;
    int    tau_s=1000;
    int    coarse_int = 1;
    int    stepper_n = 0;
    double lambda = 0.5;
    double *bias_x,*bias_y,*bias_z;
    double *bias_last_x,*bias_last_y,*bias_last_z;    
    double *av_bias_x,*av_bias_y,*av_bias_z;  
    double *av_bias_x2,*av_bias_y2,*av_bias_z2;
    double epsilon2; 
    double **hist_global_x,**hist_global_y,**hist_global_z; 
    double dL_max_x=0.0,dL_max_y,dL_max_z;
    int    recurrence_time;
    int    anneal_switch = 0;
    double stepper_3=1;
    double recurr=1.0,l_max_tt=0.0;
    double anneal_time=1000;
    double increment_alpha,increment_beta;
    double anneal_alpha=0,anneal_beta=0;
    double anneal_alpha_2=0,anneal_beta_2=0;  
    double beta=0.0,stepper_2=0;
    double recurrence_time_2=1000;
    double l_max_t,ran1,l_max_tt2=0;
    int    interval_k = 1000,stepper_4=0;
    double d_t_term,d_n_x,d_n_y,d_n_z;   
    double d_x3,d_y3,d_z3,range_x,range_y,range_z;   
    double d_t_max=0.0;
    double *d_tL;
    double av_vel_x,av_vel_y,av_vel_z;
    double **states_x,**states_y,**states_z;
    double **state_vel_x,**state_vel_y,**state_vel_z;
    double **density_grid_x,**density_grid_y,**density_grid_z;
    double *dL_last_x,*dL_last_y,*dL_last_z;
    int    apply_bias = 0;
    double delta_corr=0,l_max_tt3;
    double *rate_accum,*rate,delta_corr_av=1E-7,num_cycles=1.0;
    double sum_rate=0,en_last=0.0;
    int    time_last2=0,delta_time2=0,time_last=0.0,delta_time=0.0;
    int    timestep_kmc=1000;
    int    select_kmc,instance=1,kmc_init=0,kmc_init2=0;
    int    number_of_states;
    int    kmc_phases=0;
    int    num_kmc_states,contract=0;
    double V_bias,time_step2=10000;
    double W1=0;
    double time_range;
    
    increment_alpha = 1;
    increment_beta  = 1;
    recurrence_time = 0;
   
    FILE *fp;
    FILE *fp2;

    printf("%s\n","Here start of md");
 
    h3 = 1.0;
        
    PI = 3.14159265359;

    natoms1 = top_global->natoms;
   
    time_range   = 1E+4*((double)natoms1);   
    
    num_kmc_states = natoms1;

    printf("%d\t%lg%s\n",natoms1,time_range," natoms + time_range ");
    
    snew(at_id,natoms1+1);   
    
    for(i=1;i<=natoms1;i++) 

    {

      at_id[i] = i - 1;

      printf("%d\t%s\n",at_id[i]," at-id ");

    };  
    
    snew(restraint_sampling,natoms1+1);
    
    for(i=1;i<=natoms1;i++) {
        
          restraint_sampling[i] = 0;
          
    };    
    
    if(stat("restraint_data.dat", buf) == 0) {
   
      fp = fopen("restraint_data.dat","r");
   
      char char1[100];
 
      k = 1;

      while(fgets(char1,100,fp)!=0) {

         k++;

      };

      fclose(fp);

      number_of_restraints = k-1;
      
      for(i=1;i<=natoms1;i++) {
        
          restraint_sampling[i] = 0;
          
      };

      snew(at_id_r,number_of_restraints+1);
      snew(at_id_r2,number_of_restraints+1);
      snew(distance_restraint,number_of_restraints+1);

      fp = fopen("restraint_data.dat","r");
       
      int n1,n2;
      double d1,d2;
      
      k = 1; 
      
      while(fgets(char1,100,fp)!=0) {
        
        sscanf(char1,"%d%d",&n1,&n2);
        printf("%s\t%d\t%d\n","at_restr ",n1,n2);        
        
                 at_id_r[k] = n1;
                 at_id_r2[k] = n2;
                 restraint_sampling[n1] = 1;
                 restraint_sampling[n2] = 1;
                 
        k++;
        
      };
      
      fclose(fp);
      
   };  
   
   density_grid_x = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       density_grid_x[i] = (double *) malloc(sizeof(double)*201);
       
   };   
   
   density_grid_y = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       density_grid_y[i] = (double *) malloc(sizeof(double)*201);
       
   };  
   
   density_grid_z = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       density_grid_z[i] = (double *) malloc(sizeof(double)*201);
       
   };     
   
   hist_global_x = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       hist_global_x[i] = (double *) malloc(sizeof(double)*201);
       
   };   
   
   hist_global_y = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       hist_global_y[i] = (double *) malloc(sizeof(double)*201);
       
   };  
   
   hist_global_z = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       hist_global_z[i] = (double *) malloc(sizeof(double)*201);
       
   };       

   state_vel_x = (double **) malloc(sizeof(double)*(natoms1+1));

   for(i=1;i<=natoms1;i++) {

       state_vel_x[i] = (double *) malloc(sizeof(double)*(natoms1+1));

   };

   state_vel_y = (double **) malloc(sizeof(double)*(natoms1+1));

   for(i=1;i<=natoms1;i++) {

       state_vel_y[i] = (double *) malloc(sizeof(double)*(natoms1+1));

   };

   state_vel_z = (double **) malloc(sizeof(double)*(natoms1+1));

   for(i=1;i<=natoms1;i++) {

       state_vel_z[i] = (double *) malloc(sizeof(double)*(natoms1+1));

   };
   
   states_x = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       states_x[i] = (double *) malloc(sizeof(double)*(natoms1+1));
       
   };   
   
   states_y = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       states_y[i] = (double *) malloc(sizeof(double)*(natoms1+1));
       
   };  
   
   states_z = (double **) malloc(sizeof(double)*(natoms1+1));
   
   for(i=1;i<=natoms1;i++) {
     
       states_z[i] = (double *) malloc(sizeof(double)*(natoms1+1));
       
   };    
   
    snew(time_step,natoms1+1);
    snew(rate_accum,natoms1+1);
    snew(rate,natoms1+1);    
    snew(x_1,natoms1+1);
    snew(y_1,natoms1+1);
    snew(z_1,natoms1+1);
    snew(diff_s_x,natoms1+1);
    snew(diff_s_y,natoms1+1);
    snew(diff_s_z,natoms1+1);    
    snew(d_tL,natoms1+1);
    snew(bias_x,natoms1+1);
    snew(bias_y,natoms1+1);
    snew(bias_z,natoms1+1);    
    snew(stepper_d,natoms1+1);
    snew(flag_update,natoms1+1);
    snew(diff_s_x1,natoms1+1);
    snew(diff_s_y1,natoms1+1);
    snew(diff_s_z1,natoms1+1); 
    snew(diff_s_xc,natoms1+1);
    snew(diff_s_yc,natoms1+1);
    snew(diff_s_zc,natoms1+1); 
    snew(diff_s_x2,natoms1+1);
    snew(diff_s_y2,natoms1+1);
    snew(diff_s_z2,natoms1+1);    
    snew(probability_x1,natoms1+1);
    snew(probability_y1,natoms1+1);
    snew(probability_z1,natoms1+1);                   
    snew(dx_3,natoms1+1);
    snew(dy_3,natoms1+1);
    snew(dz_3,natoms1+1);     
    snew(dL_x,natoms1+1);
    snew(dL_y,natoms1+1);
    snew(dL_z,natoms1+1);               
    snew(diff_L_xc,natoms1+1);
    snew(diff_L_yc,natoms1+1);
    snew(diff_L_zc,natoms1+1);               
    snew(meas_x,natoms1+1);
    snew(meas_y,natoms1+1);
    snew(meas_z,natoms1+1);     
    snew(meas_x_last,natoms1+1);
    snew(meas_y_last,natoms1+1);
    snew(meas_z_last,natoms1+1);
    snew(d_xx2,natoms1+1);
    snew(d_yy2,natoms1+1);
    snew(d_zz2,natoms1+1);         
    snew(dLL_x,natoms1+1);
    snew(dLL_y,natoms1+1);
    snew(dLL_z,natoms1+1);
    snew(tau_1,natoms1+1);    
    snew(dL_last_x,natoms1+1);
    snew(dL_last_y,natoms1+1);
    snew(dL_last_z,natoms1+1);       
    
   printf("%s\n"," a3 ");    
    
    /* Check for special mdrun options */
    bRerunMD = (Flags & MD_RERUN);
    bIonize  = (Flags & MD_IONIZE);
    bFFscan  = (Flags & MD_FFSCAN);
    bAppend  = (Flags & MD_APPENDFILES);
    if (Flags & MD_RESETCOUNTERSHALFWAY)
    {
        if (ir->nsteps > 0)
        {
            /* Signal to reset the counters half the simulation steps. */
            wcycle_set_reset_counters(wcycle,ir->nsteps/2);
        }
        /* Signal to reset the counters halfway the simulation time. */
        bResetCountersHalfMaxH = (max_hours > 0);
    }

    /* md-vv uses averaged full step velocities for T-control 
       md-vv-avek uses averaged half step velocities for T-control (but full step ekin for P control)
       md uses averaged half step kinetic energies to determine temperature unless defined otherwise by GMX_EKIN_AVE_VEL; */
    bVV = EI_VV(ir->eI);
    if (bVV) /* to store the initial velocities while computing virial */
    {
        snew(cbuf,top_global->natoms);
    }
    /* all the iteratative cases - only if there are constraints */ 
    bIterations = ((IR_NPT_TROTTER(ir)) && (constr) && (!bRerunMD));
    bTrotter = (bVV && (IR_NPT_TROTTER(ir) || (IR_NVT_TROTTER(ir))));        
    
    if (bRerunMD)
    {
        /* Since we don't know if the frames read are related in any way,
         * rebuild the neighborlist at every step.
         */
        ir->nstlist       = 1;
        ir->nstcalcenergy = 1;
        nstglobalcomm     = 1;
    }

    check_ir_old_tpx_versions(cr,fplog,ir,top_global);

    nstglobalcomm = check_nstglobalcomm(fplog,cr,nstglobalcomm,ir);
    bGStatEveryStep = (nstglobalcomm == 1);

    if (!bGStatEveryStep && ir->nstlist == -1 && fplog != NULL)
    {
        fprintf(fplog,
                "To reduce the energy communication with nstlist = -1\n"
                "the neighbor list validity should not be checked at every step,\n"
                "this means that exact integration is not guaranteed.\n"
                "The neighbor list validity is checked after:\n"
                "  <n.list life time> - 2*std.dev.(n.list life time)  steps.\n"
                "In most cases this will result in exact integration.\n"
                "This reduces the energy communication by a factor of 2 to 3.\n"
                "If you want less energy communication, set nstlist > 3.\n\n");
    }

    if (bRerunMD || bFFscan)
    {
        ir->nstxtcout = 0;
    }
    groups = &top_global->groups;

    /* Initial values */
    init_md(fplog,cr,ir,oenv,&t,&t0,&state_global->lambda,&lam0,
            nrnb,top_global,&upd,
            nfile,fnm,&outf,&mdebin,
            force_vir,shake_vir,mu_tot,&bSimAnn,&vcm,state_global,Flags);

    clear_mat(total_vir);
    clear_mat(pres);
    /* Energy terms and groups */
    snew(enerd,1);
    init_enerdata(top_global->groups.grps[egcENER].nr,ir->n_flambda,enerd);
    if (DOMAINDECOMP(cr))
    {
        f = NULL;
    }
    else
    {
        snew(f,top_global->natoms);
    }

    /* Kinetic energy data */
    snew(ekind,1);
    init_ekindata(fplog,top_global,&(ir->opts),ekind);
    /* needed for iteration of constraints */
    snew(ekind_save,1);
    init_ekindata(fplog,top_global,&(ir->opts),ekind_save);
    /* Copy the cos acceleration to the groups struct */    
    ekind->cosacc.cos_accel = ir->cos_accel;

    gstat = global_stat_init(ir);
    debug_gmx();

    /* Check for polarizable models and flexible constraints */
    shellfc = init_shell_flexcon(fplog,
                                 top_global,n_flexible_constraints(constr),
                                 (ir->bContinuation || 
                                  (DOMAINDECOMP(cr) && !MASTER(cr))) ?
                                 NULL : state_global->x);

    if (DEFORM(*ir))
    {
#ifdef GMX_THREADS
        tMPI_Thread_mutex_lock(&deform_init_box_mutex);
#endif
        set_deform_reference_box(upd,
                                 deform_init_init_step_tpx,
                                 deform_init_box_tpx);
#ifdef GMX_THREADS
        tMPI_Thread_mutex_unlock(&deform_init_box_mutex);
#endif
    }

    {
        double io = compute_io(ir,top_global->natoms,groups,mdebin->ebin->nener,1);
        if ((io > 2000) && MASTER(cr))
            fprintf(stderr,
                    "\nWARNING: This run will generate roughly %.0f Mb of data\n\n",
                    io);
    }

    if (DOMAINDECOMP(cr)) {
        top = dd_init_local_top(top_global);

        snew(state,1);
        dd_init_local_state(cr->dd,state_global,state);

    //    if (DDMASTER(cr->dd) && ir->nstfout) {
            snew(f_global,state_global->natoms);
    //     }
    } else {
        if (PAR(cr)) {
            /* Initialize the particle decomposition and split the topology */
            top = split_system(fplog,top_global,ir,cr);

            pd_cg_range(cr,&fr->cg0,&fr->hcg);
            pd_at_range(cr,&a0,&a1);
        } else {
            top = gmx_mtop_generate_local_top(top_global,ir);

            a0 = 0;
            a1 = top_global->natoms;
        }

        state = partdec_init_local_state(cr,state_global);
        f_global = f;

        atoms2md(top_global,ir,0,NULL,a0,a1-a0,mdatoms);

        if (vsite) {
            set_vsite_top(vsite,top,mdatoms,cr);
        }

        if (ir->ePBC != epbcNONE && !ir->bPeriodicMols) {
            graph = mk_graph(fplog,&(top->idef),0,top_global->natoms,FALSE,FALSE);
        }

        if (shellfc) {
            make_local_shells(cr,mdatoms,shellfc);
        }

        if (ir->pull && PAR(cr)) {
            dd_make_local_pull_groups(NULL,ir->pull,mdatoms);
        }
    }

    if (DOMAINDECOMP(cr))
    {
        /* Distribute the charge groups over the nodes from the master node */
        dd_partition_system(fplog,ir->init_step,cr,TRUE,1,
                            state_global,top_global,ir,
                            state,&f,mdatoms,top,fr,
                            vsite,shellfc,constr,
                            nrnb,wcycle,FALSE);
    }

    update_mdatoms(mdatoms,state->lambda);

    if (MASTER(cr))
    {
        if (opt2bSet("-cpi",nfile,fnm))
        {
            /* Update mdebin with energy history if appending to output files */
            if ( Flags & MD_APPENDFILES )
            {
                restore_energyhistory_from_state(mdebin,&state_global->enerhist);
            }
            else
            {
                /* We might have read an energy history from checkpoint,
                 * free the allocated memory and reset the counts.
                 */
                done_energyhistory(&state_global->enerhist);
                init_energyhistory(&state_global->enerhist);
            }
        }
        /* Set the initial energy history in state by updating once */
        update_energyhistory(&state_global->enerhist,mdebin);
    }	

    if ((state->flags & (1<<estLD_RNG)) && (Flags & MD_READ_RNG)) {
        /* Set the random state if we read a checkpoint file */
        set_stochd_state(upd,state);
    }

    /* Initialize constraints */
    if (constr) {
        if (!DOMAINDECOMP(cr))
            set_constraints(constr,top,ir,mdatoms,cr);
    }

    /* Check whether we have to GCT stuff */
    bTCR = ftp2bSet(efGCT,nfile,fnm);
    if (bTCR) {
        if (MASTER(cr)) {
            fprintf(stderr,"Will do General Coupling Theory!\n");
        }
        gnx = top_global->mols.nr;
        snew(grpindex,gnx);
        for(i=0; (i<gnx); i++) {
            grpindex[i] = i;
        }
    }

    if (repl_ex_nst > 0)
    {
        /* We need to be sure replica exchange can only occur
         * when the energies are current */
        check_nst_param(fplog,cr,"nstcalcenergy",ir->nstcalcenergy,
                        "repl_ex_nst",&repl_ex_nst);
        /* This check needs to happen before inter-simulation
         * signals are initialized, too */
    }
    if (repl_ex_nst > 0 && MASTER(cr))
        repl_ex = init_replica_exchange(fplog,cr->ms,state_global,ir,
                                        repl_ex_nst,repl_ex_seed);

    if (!ir->bContinuation && !bRerunMD)
    {
        if (mdatoms->cFREEZE && (state->flags & (1<<estV)))
        {
            /* Set the velocities of frozen particles to zero */
            for(i=mdatoms->start; i<mdatoms->start+mdatoms->homenr; i++)
            {
                for(m=0; m<DIM; m++)
                {
                    if (ir->opts.nFreeze[mdatoms->cFREEZE[i]][m])
                    {
                        state->v[i][m] = 0;
                    }
                }
            }
        }

        if (constr)
        {
            /* Constrain the initial coordinates and velocities */
            do_constrain_first(fplog,constr,ir,mdatoms,state,f,
                               graph,cr,nrnb,fr,top,shake_vir);
        }
        if (vsite)
        {
            /* Construct the virtual sites for the initial configuration */
            construct_vsites(fplog,vsite,state->x,nrnb,ir->delta_t,NULL,
                             top->idef.iparams,top->idef.il,
                             fr->ePBC,fr->bMolPBC,graph,cr,state->box);
        }
    }

    debug_gmx();
  
    /* I'm assuming we need global communication the first time! MRS */
    cglo_flags = (CGLO_TEMPERATURE | CGLO_GSTAT
                  | (bVV ? CGLO_PRESSURE:0)
                  | (bVV ? CGLO_CONSTRAINT:0)
                  | (bRerunMD ? CGLO_RERUNMD:0)
                  | ((Flags & MD_READ_EKIN) ? CGLO_READEKIN:0));
    
    bSumEkinhOld = FALSE;
    compute_globals(fplog,gstat,cr,ir,fr,ekind,state,state_global,mdatoms,nrnb,vcm,
                    wcycle,enerd,force_vir,shake_vir,total_vir,pres,mu_tot,
                    constr,NULL,FALSE,state->box,
                    top_global,&pcurr,top_global->natoms,&bSumEkinhOld,cglo_flags);
    if (ir->eI == eiVVAK) {
        /* a second call to get the half step temperature initialized as well */ 
        /* we do the same call as above, but turn the pressure off -- internally to 
           compute_globals, this is recognized as a velocity verlet half-step 
           kinetic energy calculation.  This minimized excess variables, but 
           perhaps loses some logic?*/
        
        compute_globals(fplog,gstat,cr,ir,fr,ekind,state,state_global,mdatoms,nrnb,vcm,
                        wcycle,enerd,force_vir,shake_vir,total_vir,pres,mu_tot,
                        constr,NULL,FALSE,state->box,
                        top_global,&pcurr,top_global->natoms,&bSumEkinhOld,
                        cglo_flags &~ CGLO_PRESSURE);
    }
    
    /* Calculate the initial half step temperature, and save the ekinh_old */
    if (!(Flags & MD_STARTFROMCPT)) 
    {
        for(i=0; (i<ir->opts.ngtc); i++) 
        {
            copy_mat(ekind->tcstat[i].ekinh,ekind->tcstat[i].ekinh_old);
        } 
    }
    if (ir->eI != eiVV) 
    {
        enerd->term[F_TEMP] *= 2; /* result of averages being done over previous and current step,
                                     and there is no previous step */
    }
    temp0 = enerd->term[F_TEMP];
    
    /* if using an iterative algorithm, we need to create a working directory for the state. */
    if (bIterations) 
    {
            bufstate = init_bufstate(state);
    }
    if (bFFscan) 
    {
        snew(xcopy,state->natoms);
        snew(vcopy,state->natoms);
        copy_rvecn(state->x,xcopy,0,state->natoms);
        copy_rvecn(state->v,vcopy,0,state->natoms);
        copy_mat(state->box,boxcopy);
    } 
    
    /* need to make an initiation call to get the Trotter variables set, as well as other constants for non-trotter
       temperature control */
    trotter_seq = init_npt_vars(ir,state,&MassQ,bTrotter);
    
    if (MASTER(cr))
    {
        if (constr && !ir->bContinuation && ir->eConstrAlg == econtLINCS)
        {
            fprintf(fplog,
                    "RMS relative constraint deviation after constraining: %.2e\n",
                    constr_rmsd(constr,FALSE));
        }
        fprintf(fplog,"Initial temperature: %g K\n",enerd->term[F_TEMP]);
        if (bRerunMD)
        {
            fprintf(stderr,"starting md rerun '%s', reading coordinates from"
                    " input trajectory '%s'\n\n",
                    *(top_global->name),opt2fn("-rerun",nfile,fnm));
            if (bVerbose)
            {
                fprintf(stderr,"Calculated time to finish depends on nsteps from "
                        "run input file,\nwhich may not correspond to the time "
                        "needed to process input trajectory.\n\n");
            }
        }
        else
        {
            char tbuf[20];
            fprintf(stderr,"starting mdrun '%s'\n",
                    *(top_global->name));
            if (ir->nsteps >= 0)
            {
                sprintf(tbuf,"%8.1f",(ir->init_step+ir->nsteps)*ir->delta_t);
            }
            else
            {
                sprintf(tbuf,"%s","infinite");
            }
            if (ir->init_step > 0)
            {
                fprintf(stderr,"%s steps, %s ps (continuing from step %s, %8.1f ps).\n",
                        gmx_step_str(ir->init_step+ir->nsteps,sbuf),tbuf,
                        gmx_step_str(ir->init_step,sbuf2),
                        ir->init_step*ir->delta_t);
            }
            else
            {
                fprintf(stderr,"%s steps, %s ps.\n",
                        gmx_step_str(ir->nsteps,sbuf),tbuf);
            }
        }
        fprintf(fplog,"\n");
    }

    /* Set and write start time */
    runtime_start(runtime);
    print_date_and_time(fplog,cr->nodeid,"Started mdrun",runtime);
    wallcycle_start(wcycle,ewcRUN);
    if (fplog)
        fprintf(fplog,"\n");

    /* safest point to do file checkpointing is here.  More general point would be immediately before integrator call */
#ifdef GMX_FAHCORE
    chkpt_ret=fcCheckPointParallel( cr->nodeid,
                                    NULL,0);
    if ( chkpt_ret == 0 ) 
        gmx_fatal( 3,__FILE__,__LINE__, "Checkpoint error on step %d\n", 0 );
#endif

    debug_gmx();
    /***********************************************************
     *
     *             Loop over MD steps 
     *
     ************************************************************/

    /* if rerunMD then read coordinates and velocities from input trajectory */
    if (bRerunMD)
    {
        if (getenv("GMX_FORCE_UPDATE"))
        {
            bForceUpdate = TRUE;
        }

        rerun_fr.natoms = 0;
        if (MASTER(cr))
        {
            bNotLastFrame = read_first_frame(oenv,&status,
                                             opt2fn("-rerun",nfile,fnm),
                                             &rerun_fr,TRX_NEED_X | TRX_READ_V);
            if (rerun_fr.natoms != top_global->natoms)
            {
                gmx_fatal(FARGS,
                          "Number of atoms in trajectory (%d) does not match the "
                          "run input file (%d)\n",
                          rerun_fr.natoms,top_global->natoms);
            }
            if (ir->ePBC != epbcNONE)
            {
                if (!rerun_fr.bBox)
                {
                    gmx_fatal(FARGS,"Rerun trajectory frame step %d time %f does not contain a box, while pbc is used",rerun_fr.step,rerun_fr.time);
                }
                if (max_cutoff2(ir->ePBC,rerun_fr.box) < sqr(fr->rlistlong))
                {
                    gmx_fatal(FARGS,"Rerun trajectory frame step %d time %f has too small box dimensions",rerun_fr.step,rerun_fr.time);
                }
            }
        }

        if (PAR(cr))
        {
            rerun_parallel_comm(cr,&rerun_fr,&bNotLastFrame);
        }

        if (ir->ePBC != epbcNONE)
        {
            /* Set the shift vectors.
             * Necessary here when have a static box different from the tpr box.
             */
            calc_shifts(rerun_fr.box,fr->shift_vec);
        }
    }

    /* loop over MD steps or if rerunMD to end of input trajectory */
    bFirstStep = TRUE;
    /* Skip the first Nose-Hoover integration when we get the state from tpx */
    bStateFromTPX = !opt2bSet("-cpi",nfile,fnm);
    bInitStep = bFirstStep && (bStateFromTPX || bVV);
    bStartingFromCpt = (Flags & MD_STARTFROMCPT) && bInitStep;
    bLastStep    = FALSE;
    bSumEkinhOld = FALSE;
    bExchanged   = FALSE;

    init_global_signals(&gs,cr,ir,repl_ex_nst);

    step = ir->init_step;
    step_rel = 0;

    if (ir->nstlist == -1)
    {
        init_nlistheuristics(&nlh,bGStatEveryStep,step);
    }

    if (MULTISIM(cr) && (repl_ex_nst <=0 ))
    {
        /* check how many steps are left in other sims */
        multisim_nsteps=get_multisim_nsteps(cr, ir->nsteps);
    }


    /* and stop now if we should */
    bLastStep = (bRerunMD || (ir->nsteps >= 0 && step_rel > ir->nsteps) ||
                 ((multisim_nsteps >= 0) && (step_rel >= multisim_nsteps )));
    while (!bLastStep || (bRerunMD && bNotLastFrame)) {

        wallcycle_start(wcycle,ewcSTEP);

        GMX_MPE_LOG(ev_timestep1);

        if (bRerunMD) {
            if (rerun_fr.bStep) {
                step = rerun_fr.step;
                step_rel = step - ir->init_step;
            }
            if (rerun_fr.bTime) {
                t = rerun_fr.time;
            }
            else
            {
                t = step;
            }
        } 
        else 
        {
            bLastStep = (step_rel == ir->nsteps);
            t = t0 + step*ir->delta_t;
        }

        if (ir->efep != efepNO)
        {
            if (bRerunMD && rerun_fr.bLambda && (ir->delta_lambda!=0))
            {
                state_global->lambda = rerun_fr.lambda;
            }
            else
            {
                state_global->lambda = lam0 + step*ir->delta_lambda;
            }
            state->lambda = state_global->lambda;
            bDoDHDL = do_per_step(step,ir->nstdhdl);
        }

        if (bSimAnn) 
        {
            update_annealing_target_temp(&(ir->opts),t);
        }

        if (bRerunMD)
        {
            if (!(DOMAINDECOMP(cr) && !MASTER(cr)))
            {
                for(i=0; i<state_global->natoms; i++)
                {
                    copy_rvec(rerun_fr.x[i],state_global->x[i]);
                }
                if (rerun_fr.bV)
                {
                    for(i=0; i<state_global->natoms; i++)
                    {
                        copy_rvec(rerun_fr.v[i],state_global->v[i]);
                    }
                }
                else
                {
                    for(i=0; i<state_global->natoms; i++)
                    {
                        clear_rvec(state_global->v[i]);
                    }
                    if (bRerunWarnNoV)
                    {
                        fprintf(stderr,"\nWARNING: Some frames do not contain velocities.\n"
                                "         Ekin, temperature and pressure are incorrect,\n"
                                "         the virial will be incorrect when constraints are present.\n"
                                "\n");
                        bRerunWarnNoV = FALSE;
                    }
                }
            }
            copy_mat(rerun_fr.box,state_global->box);
            copy_mat(state_global->box,state->box);

            if (vsite && (Flags & MD_RERUN_VSITE))
            {
                if (DOMAINDECOMP(cr))
                {
                    gmx_fatal(FARGS,"Vsite recalculation with -rerun is not implemented for domain decomposition, use particle decomposition");
                }
                if (graph)
                {
                    /* Following is necessary because the graph may get out of sync
                     * with the coordinates if we only have every N'th coordinate set
                     */
                    mk_mshift(fplog,graph,fr->ePBC,state->box,state->x);
                    shift_self(graph,state->box,state->x);
                }
                construct_vsites(fplog,vsite,state->x,nrnb,ir->delta_t,state->v,
                                 top->idef.iparams,top->idef.il,
                                 fr->ePBC,fr->bMolPBC,graph,cr,state->box);
                if (graph)
                {
                    unshift_self(graph,state->box,state->x);
                }
            }
        }

        /* Stop Center of Mass motion */
        bStopCM = (ir->comm_mode != ecmNO && do_per_step(step,ir->nstcomm));

        /* Copy back starting coordinates in case we're doing a forcefield scan */
        if (bFFscan)
        {
            for(ii=0; (ii<state->natoms); ii++)
            {
                copy_rvec(xcopy[ii],state->x[ii]);
                copy_rvec(vcopy[ii],state->v[ii]);
            }
            copy_mat(boxcopy,state->box);
        }

        if (bRerunMD)
        {
            /* for rerun MD always do Neighbour Searching */
            bNS = (bFirstStep || ir->nstlist != 0);
            bNStList = bNS;
        }
        else
        {
            /* Determine whether or not to do Neighbour Searching and LR */
            bNStList = (ir->nstlist > 0  && step % ir->nstlist == 0);
            
            bNS = (bFirstStep || bExchanged || bNStList ||
                   (ir->nstlist == -1 && nlh.nabnsb > 0));

            if (bNS && ir->nstlist == -1)
            {
                set_nlistheuristics(&nlh,bFirstStep || bExchanged,step);
            }
        } 

        /* check whether we should stop because another simulation has 
           stopped. */
        if (MULTISIM(cr))
        {
            if ( (multisim_nsteps >= 0) &&  (step_rel >= multisim_nsteps)  &&  
                 (multisim_nsteps != ir->nsteps) )  
            {
                if (bNS)
                {
                    if (MASTER(cr))
                    {
                        fprintf(stderr, 
                                "Stopping simulation %d because another one has finished\n",
                                cr->ms->sim);
                    }
                    bLastStep=TRUE;
                    gs.sig[eglsCHKPT] = 1;
                }
            }
        }

        /* < 0 means stop at next step, > 0 means stop at next NS step */
        if ( (gs.set[eglsSTOPCOND] < 0 ) ||
             ( (gs.set[eglsSTOPCOND] > 0 ) && ( bNS || ir->nstlist==0)) )
        {
            bLastStep = TRUE;
        }

        /* Determine whether or not to update the Born radii if doing GB */
        bBornRadii=bFirstStep;
        if (ir->implicit_solvent && (step % ir->nstgbradii==0))
        {
            bBornRadii=TRUE;
        }
        
        do_log = do_per_step(step,ir->nstlog) || bFirstStep || bLastStep;
        do_verbose = bVerbose &&
                  (step % stepout == 0 || bFirstStep || bLastStep);

        if (bNS && !(bFirstStep && ir->bContinuation && !bRerunMD))
        {
            if (bRerunMD)
            {
                bMasterState = TRUE;
            }
            else
            {
                bMasterState = FALSE;
                /* Correct the new box if it is too skewed */
                if (DYNAMIC_BOX(*ir))
                {
                    if (correct_box(fplog,step,state->box,graph))
                    {
                        bMasterState = TRUE;
                    }
                }
                if (DOMAINDECOMP(cr) && bMasterState)
                {
                    dd_collect_state(cr->dd,state,state_global);
                }
            }

            if (DOMAINDECOMP(cr))
            {
                /* Repartition the domain decomposition */
                wallcycle_start(wcycle,ewcDOMDEC);
                dd_partition_system(fplog,step,cr,
                                    bMasterState,nstglobalcomm,
                                    state_global,top_global,ir,
                                    state,&f,mdatoms,top,fr,
                                    vsite,shellfc,constr,
                                    nrnb,wcycle,do_verbose);
                wallcycle_stop(wcycle,ewcDOMDEC);
                /* If using an iterative integrator, reallocate space to match the decomposition */
            }
        }

        if (MASTER(cr) && do_log && !bFFscan)
        {
            print_ebin_header(fplog,step,t,state->lambda);
        }

        if (ir->efep != efepNO)
        {
            update_mdatoms(mdatoms,state->lambda); 
        }

        if (bRerunMD && rerun_fr.bV)
        {
            
            /* We need the kinetic energy at minus the half step for determining
             * the full step kinetic energy and possibly for T-coupling.*/
            /* This may not be quite working correctly yet . . . . */
            compute_globals(fplog,gstat,cr,ir,fr,ekind,state,state_global,mdatoms,nrnb,vcm,
                            wcycle,enerd,NULL,NULL,NULL,NULL,mu_tot,
                            constr,NULL,FALSE,state->box,
                            top_global,&pcurr,top_global->natoms,&bSumEkinhOld,
                            CGLO_RERUNMD | CGLO_GSTAT | CGLO_TEMPERATURE);
        }
        clear_mat(force_vir);
        
        /* Ionize the atoms if necessary */
        if (bIonize)
        {
            ionize(fplog,oenv,mdatoms,top_global,t,ir,state->x,state->v,
                   mdatoms->start,mdatoms->start+mdatoms->homenr,state->box,cr);
        }
        
        /* Update force field in ffscan program */
        if (bFFscan)
        {
            if (update_forcefield(fplog,
                                  nfile,fnm,fr,
                                  mdatoms->nr,state->x,state->box)) {
                if (gmx_parallel_env_initialized())
                {
                    gmx_finalize();
                }
                exit(0);
            }
        }

        GMX_MPE_LOG(ev_timestep2);

        /* We write a checkpoint at this MD step when:
         * either at an NS step when we signalled through gs,
         * or at the last step (but not when we do not want confout),
         * but never at the first step or with rerun.
         */
        bCPT = (((gs.set[eglsCHKPT] && (bNS || ir->nstlist == 0)) ||
                 (bLastStep && (Flags & MD_CONFOUT))) &&
                step > ir->init_step && !bRerunMD);
        if (bCPT)
        {
            gs.set[eglsCHKPT] = 0;
        }

        /* Determine the energy and pressure:
         * at nstcalcenergy steps and at energy output steps (set below).
         */
        bNstEner = do_per_step(step,ir->nstcalcenergy);
        bCalcEnerPres =
            (bNstEner ||
             (ir->epc != epcNO && do_per_step(step,ir->nstpcouple)));

        /* Do we need global communication ? */
        bGStat = (bCalcEnerPres || bStopCM ||
                  do_per_step(step,nstglobalcomm) ||
                  (ir->nstlist == -1 && !bRerunMD && step >= nlh.step_nscheck));

        do_ene = (do_per_step(step,ir->nstenergy) || bLastStep);

        if (do_ene || do_log)
        {
            bCalcEnerPres = TRUE;
            bGStat        = TRUE;
        }
        
        /* these CGLO_ options remain the same throughout the iteration */
        cglo_flags = ((bRerunMD ? CGLO_RERUNMD : 0) |
                      (bStopCM ? CGLO_STOPCM : 0) |
                      (bGStat ? CGLO_GSTAT : 0)
            );
        
        force_flags = (GMX_FORCE_STATECHANGED |
                       ((DYNAMIC_BOX(*ir) || bRerunMD) ? GMX_FORCE_DYNAMICBOX : 0) |
                       GMX_FORCE_ALLFORCES |
                       (bNStList ? GMX_FORCE_DOLR : 0) |
                       GMX_FORCE_SEPLRF |
                       (bCalcEnerPres ? GMX_FORCE_VIRIAL : 0) |
                       (bDoDHDL ? GMX_FORCE_DHDL : 0)
            );
        
        if (shellfc)
        {
            /* Now is the time to relax the shells */
            count=relax_shell_flexcon(fplog,cr,bVerbose,bFFscan ? step+1 : step,
                                      ir,bNS,force_flags,
                                      bStopCM,top,top_global,
                                      constr,enerd,fcd,
                                      state,f,force_vir,mdatoms,
                                      nrnb,wcycle,graph,groups,
                                      shellfc,fr,bBornRadii,t,mu_tot,
                                      state->natoms,&bConverged,vsite,
                                      outf->fp_field);
            tcount+=count;

            if (bConverged)
            {
                nconverged++;
            }
        }
        else
        {
            /* The coordinates (x) are shifted (to get whole molecules)
             * in do_force.
             * This is parallellized as well, and does communication too. 
             * Check comments in sim_util.c
             */
        
            do_force(fplog,cr,ir,step,nrnb,wcycle,top,top_global,groups,
                     state->box,state->x,&state->hist,
                     f,force_vir,mdatoms,enerd,fcd,
                     state->lambda,graph,
                     fr,vsite,mu_tot,t,outf->fp_field,ed,bBornRadii,
                     (bNS ? GMX_FORCE_NS : 0) | force_flags);
        }
    
        GMX_BARRIER(cr->mpi_comm_mygroup);
        
        if (bTCR)
        {
            mu_aver = calc_mu_aver(cr,state->x,mdatoms->chargeA,
                                   mu_tot,&top_global->mols,mdatoms,gnx,grpindex);
        }
        
        if (bTCR && bFirstStep)
        {
            tcr=init_coupling(fplog,nfile,fnm,cr,fr,mdatoms,&(top->idef));
            fprintf(fplog,"Done init_coupling\n"); 
            fflush(fplog);
        }
        
        if (bVV && !bStartingFromCpt && !bRerunMD)

	  	  
        /*  ############### START FIRST UPDATE HALF-STEP FOR VV METHODS############### */
        {
            if (ir->eI==eiVV && bInitStep) 
            {
                /* if using velocity verlet with full time step Ekin,
                 * take the first half step only to compute the 
                 * virial for the first step. From there,
                 * revert back to the initial coordinates
                 * so that the input is actually the initial step.
                 */
                copy_rvecn(state->v,cbuf,0,state->natoms); /* should make this better for parallelizing? */
            } else {
                /* this is for NHC in the Ekin(t+dt/2) version of vv */
                trotter_update(ir,step,ekind,enerd,state,total_vir,mdatoms,&MassQ,trotter_seq,ettTSEQ1);            
            }

            update_coords(fplog,step,ir,mdatoms,state,
                          f,fr->bTwinRange && bNStList,fr->f_twin,fcd,
                          ekind,M,wcycle,upd,bInitStep,etrtVELOCITY1,
                          cr,nrnb,constr,&top->idef);
            
            if (bIterations)
            {
                gmx_iterate_init(&iterate,bIterations && !bInitStep);
            }
            /* for iterations, we save these vectors, as we will be self-consistently iterating
               the calculations */

            /*#### UPDATE EXTENDED VARIABLES IN TROTTER FORMULATION */
            
            /* save the state */
            if (bIterations && iterate.bIterate) { 
                copy_coupling_state(state,bufstate,ekind,ekind_save,&(ir->opts));
            }
            
            bFirstIterate = TRUE;
            while (bFirstIterate || (bIterations && iterate.bIterate))
            {
                if (bIterations && iterate.bIterate) 
                {
                    copy_coupling_state(bufstate,state,ekind_save,ekind,&(ir->opts));
                    if (bFirstIterate && bTrotter) 
                    {
                        /* The first time through, we need a decent first estimate
                           of veta(t+dt) to compute the constraints.  Do
                           this by computing the box volume part of the
                           trotter integration at this time. Nothing else
                           should be changed by this routine here.  If
                           !(first time), we start with the previous value
                           of veta.  */
                        
                        veta_save = state->veta;
                        trotter_update(ir,step,ekind,enerd,state,total_vir,mdatoms,&MassQ,trotter_seq,ettTSEQ0);
                        vetanew = state->veta;
                        state->veta = veta_save;
                    } 
                } 
                
                bOK = TRUE;
                if ( !bRerunMD || rerun_fr.bV || bForceUpdate) {  /* Why is rerun_fr.bV here?  Unclear. */
                    dvdl = 0;
                    
                    update_constraints(fplog,step,&dvdl,ir,ekind,mdatoms,state,graph,f,
                                       &top->idef,shake_vir,NULL,
                                       cr,nrnb,wcycle,upd,constr,
                                       bInitStep,TRUE,bCalcEnerPres,vetanew);
                    
                    if (!bOK && !bFFscan)
                    {
                        gmx_fatal(FARGS,"Constraint error: Shake, Lincs or Settle could not solve the constrains");
                    }
                    
                } 
                else if (graph)
                { /* Need to unshift here if a do_force has been
                     called in the previous step */
                    unshift_self(graph,state->box,state->x);
                }
                
                
                /* if VV, compute the pressure and constraints */
                /* For VV2, we strictly only need this if using pressure
                 * control, but we really would like to have accurate pressures
                 * printed out.
                 * Think about ways around this in the future?
                 * For now, keep this choice in comments.
                 */
                /*bPres = (ir->eI==eiVV || IR_NPT_TROTTER(ir)); */
                    /*bTemp = ((ir->eI==eiVV &&(!bInitStep)) || (ir->eI==eiVVAK && IR_NPT_TROTTER(ir)));*/
                bPres = TRUE;
                bTemp = ((ir->eI==eiVV &&(!bInitStep)) || (ir->eI==eiVVAK));
                compute_globals(fplog,gstat,cr,ir,fr,ekind,state,state_global,mdatoms,nrnb,vcm,
                                wcycle,enerd,force_vir,shake_vir,total_vir,pres,mu_tot,
                                constr,NULL,FALSE,state->box,
                                top_global,&pcurr,top_global->natoms,&bSumEkinhOld,
                                cglo_flags 
                                | CGLO_ENERGY 
                                | (bTemp ? CGLO_TEMPERATURE:0) 
                                | (bPres ? CGLO_PRESSURE : 0) 
                                | (bPres ? CGLO_CONSTRAINT : 0)
                                | ((bIterations && iterate.bIterate) ? CGLO_ITERATE : 0)  
                                | (bFirstIterate ? CGLO_FIRSTITERATE : 0)
                                | CGLO_SCALEEKIN 
                    );
                /* explanation of above: 
                   a) We compute Ekin at the full time step
                   if 1) we are using the AveVel Ekin, and it's not the
                   initial step, or 2) if we are using AveEkin, but need the full
                   time step kinetic energy for the pressure (always true now, since we want accurate statistics).
                   b) If we are using EkinAveEkin for the kinetic energy for the temperture control, we still feed in 
                   EkinAveVel because it's needed for the pressure */
                
                /* temperature scaling and pressure scaling to produce the extended variables at t+dt */
                if (!bInitStep) 
                {
                    if (bTrotter)
                    {
                        trotter_update(ir,step,ekind,enerd,state,total_vir,mdatoms,&MassQ,trotter_seq,ettTSEQ2);
                    } 
                    else 
                    {
                        update_tcouple(fplog,step,ir,state,ekind,wcycle,upd,&MassQ,mdatoms);
                    }
                }
                
                if (bIterations &&
                    done_iterating(cr,fplog,step,&iterate,bFirstIterate,
                                   state->veta,&vetanew)) 
                {
                    break;
                }
                bFirstIterate = FALSE;
            }

            if (bTrotter && !bInitStep) {
                copy_mat(shake_vir,state->svir_prev);
                copy_mat(force_vir,state->fvir_prev);
                if (IR_NVT_TROTTER(ir) && ir->eI==eiVV) {
                    /* update temperature and kinetic energy now that step is over - this is the v(t+dt) point */
                    enerd->term[F_TEMP] = sum_ekin(&(ir->opts),ekind,NULL,(ir->eI==eiVV),FALSE,FALSE);
                    enerd->term[F_EKIN] = trace(ekind->ekin);
                }
            }
            /* if it's the initial step, we performed this first step just to get the constraint virial */
            if (bInitStep && ir->eI==eiVV) {
                copy_rvecn(cbuf,state->v,0,state->natoms);
            }
            
            if (fr->bSepDVDL && fplog && do_log) 
            {
                fprintf(fplog,sepdvdlformat,"Constraint",0.0,dvdl);
            }
            enerd->term[F_DHDL_CON] += dvdl;
            
            GMX_MPE_LOG(ev_timestep1);
        }
    
        /* MRS -- now done iterating -- compute the conserved quantity */
        if (bVV) {
            saved_conserved_quantity = compute_conserved_from_auxiliary(ir,state,&MassQ);
            if (ir->eI==eiVV) 
            {
                last_ekin = enerd->term[F_EKIN]; /* does this get preserved through checkpointing? */
            }
            if ((ir->eDispCorr != edispcEnerPres) && (ir->eDispCorr != edispcAllEnerPres)) 
            {
                saved_conserved_quantity -= enerd->term[F_DISPCORR];
            }
        }       
        
            if(DOMAINDECOMP(cr)){

                 dd_collect_vec(cr->dd,state,f,f_global);
                 dd_collect_state(cr->dd,state,state_global);

            };

            stepper++;

            if(MASTER(cr))
	      
            {      
	      
   if(stepper == 1 || recurrence_time == 1) {              
 
   dL_max_x = 0.0;
   //    dL_max_y = 0.0;
   //    dL_max_z = 0.0;     
     
   if(stepper == 1) { 
     
       contract = 0;
     
       en_last = enerd->term[F_EPOT];  
       number_of_states = 0;  

       h3 = 0.0;

       for(i=1;i<=natoms1;i++) {

         h3 += sqrt(pow(state_global->v[at_id[i]][0],2) + pow(state_global->v[at_id[i]][1],2) + pow(state_global->v[at_id[i]][2],2));

       };

       h3 /= (double)natoms1;
 
     }       
       
     for(i=1;i<=natoms1;i++) {        
	
           meas_x[i]      = -1;
           meas_y[i]      = -1;
           meas_z[i]      = -1;
           meas_x_last[i] = -1;
           meas_y_last[i] = -1;
           meas_z_last[i] = -1;
 
           x_1[i] = 0.0;
           y_1[i] = 0.0;
           z_1[i] = 0.0;  

           dLL_x[i] = 0.0;
           dLL_y[i] = 0.0;
           dLL_z[i] = 0.0;           
	   
	   dL_last_x[i] = 0.0;
	   dL_last_y[i] = 0.0;
	   dL_last_z[i] = 0.0;
           
           diff_s_xc[i] = 0.0;
           diff_s_yc[i] = 0.0;
           diff_s_zc[i] = 0.0;                                          
           
           diff_s_x1[i] = 0.0;
           diff_s_y1[i] = 0.0;
           diff_s_z1[i] = 0.0;  
           
           diff_s_x2[i] = 0.0;
           diff_s_y2[i] = 0.0;
           diff_s_z2[i] = 0.0;    
        
           diff_s_x[i] = 0.0;
           diff_s_y[i] = 0.0;
           diff_s_z[i] = 0.0;  
	   
	   diff_L_xc[i] = 0.0;
	   diff_L_yc[i] = 0.0;	   
	   diff_L_zc[i] = 0.0;
	   
           tau_1[i] = 0.0;     
           
           bias_x[i] = 0.0;
           bias_y[i] = 0.0;
           bias_z[i] = 0.0;	   
	  
	 recurrence_time = 0; 
	 
         for(k=0;k<=100;k++) {
	   
	     hist_global_x[i][k] = 0.0;
	     hist_global_y[i][k] = 0.0;
	     hist_global_z[i][k] = 0.0;	
	     
	     density_grid_x[i][k] = 0.0;
	     density_grid_y[i][k] = 0.0;
	     density_grid_z[i][k] = 0.0;		     
	   
	 };	 
	
       if(stepper == 1) { 

	 for(k=0;k<=num_kmc_states;k++) {
	   
	   states_x[i][k] = state_global->x[at_id[i]][0];
	   states_y[i][k] = state_global->x[at_id[i]][1];	   
	   states_z[i][k] = state_global->x[at_id[i]][2];
	  
           state_vel_x[i][k] = state_global->v[at_id[i]][0];
           state_vel_y[i][k] = state_global->v[at_id[i]][1];
           state_vel_z[i][k] = state_global->v[at_id[i]][2];

	   rate[k]        = 0.0;
       
           rate_accum[k]  = 0.0;
       
           time_step[k]   = time_range;
	   
	   number_of_states = 1;
	   
	  }; 	  
	 };
       };
     };  

if(recurrence_time == 1 || stepper == 1) {      
     
av_x = 0.0;
av_y = 0.0;
av_z = 0.0;


}

l_max_tt = 0.0;
l_max_tt2 = 0.0;

d_t3 = 0.0;

double bias_xx,bias_yy,bias_zz;

bias_xx = 0.0;
bias_yy = 0.0;
bias_zz = 0.0;

V_bias  = 0.0;


for(i=1;i<=natoms1;i++){ 
    
        tau_1[i]    += 1.0;
        
        dLL_x[i]     += state_global->v[at_id[i]][0]*(state_global->x[at_id[i]][0] - x_1[i]/tau_1[i]);
        dLL_y[i]     += state_global->v[at_id[i]][1]*(state_global->x[at_id[i]][1] - y_1[i]/tau_1[i]);
        dLL_z[i]     += state_global->v[at_id[i]][2]*(state_global->x[at_id[i]][2] - z_1[i]/tau_1[i]);	
	
        diff_s_x1[i] = dLL_x[i]; 
        diff_s_y1[i] = dLL_y[i];
        diff_s_z1[i] = dLL_z[i];
        
        diff_s_x[i] += dLL_x[i];
        diff_s_y[i] += dLL_y[i];
        diff_s_z[i] += dLL_z[i];
	
    if(stepper%1000 == 0) {
      
        diff_s_x2[i] = dLL_x[i];
        diff_s_y2[i] = dLL_y[i];	
        diff_s_z2[i] = dLL_z[i];
	
    }	
        
    if(diff_s_x1[i] - diff_s_x[i]/tau_1[i] != 0.0 && diff_s_x2[i] - diff_s_x[i]/tau_1[i] != 0.0) { 
        diff_s_xc[i] += (diff_s_x2[i] - diff_s_x[i]/tau_1[i])*(diff_s_x1[i] - diff_s_x[i]/tau_1[i])/
        (sqrt(pow(diff_s_x2[i] - diff_s_x[i]/tau_1[i],2))*sqrt(pow(diff_s_x1[i] - diff_s_x[i]/tau_1[i],2)));
        
        diff_L_xc[i] = (diff_s_x1[i] - diff_s_x[i]/tau_1[i]);    
        
    };
        
    if(diff_s_y1[i] - diff_s_y[i]/tau_1[i] != 0.0 && diff_s_y2[i] - diff_s_y[i]/tau_1[i] != 0.0) {
        diff_s_yc[i] += (diff_s_y2[i] - diff_s_y[i]/tau_1[i])*(diff_s_y1[i] - diff_s_y[i]/tau_1[i])/
        (sqrt(pow(diff_s_y2[i] - diff_s_y[i]/tau_1[i],2))*sqrt(pow(diff_s_y1[i] - diff_s_y[i]/tau_1[i],2)));
        
        diff_L_yc[i] = (diff_s_y1[i] - diff_s_y[i]/tau_1[i]);
        
    };
        
    if(diff_s_z1[i] - diff_s_z[i]/tau_1[i] != 0.0 && diff_s_z2[i] - diff_s_z[i]/tau_1[i] != 0.0) {
        diff_s_zc[i] += (diff_s_z2[i] - diff_s_z[i]/tau_1[i])*(diff_s_z1[i] - diff_s_z[i]/tau_1[i])/
        (sqrt(pow(diff_s_z2[i] - diff_s_z[i]/tau_1[i],2))*sqrt(pow(diff_s_z1[i] - diff_s_z[i]/tau_1[i],2)));
        
        diff_L_zc[i] = (diff_s_z1[i] - diff_s_z[i]/tau_1[i]);
        
    };
    
    bias_x[i] = (diff_s_xc[i]/tau_1[i]);
    bias_y[i] = (diff_s_yc[i]/tau_1[i]);
    bias_z[i] = (diff_s_zc[i]/tau_1[i]);          
    
    d_t       = bias_x[i] + bias_y[i] + bias_z[i];
    
    l_max_tt += d_t;    
    
    d_x = dLL_x[i]*(1+beta*(bias_x[i]))*exp(-beta*bias_x[i]);
    d_y = dLL_y[i]*(1+beta*(bias_y[i]))*exp(-beta*bias_y[i]);    
    d_z = dLL_z[i]*(1+beta*(bias_z[i]))*exp(-beta*bias_z[i]);    
    
    d_x = sqrt(pow(d_x,2));
    d_y = sqrt(pow(d_y,2));    
    d_z = sqrt(pow(d_z,2));
    
    if(d_x > dL_max_x) dL_max_x = d_x;
    if(d_y > dL_max_x) dL_max_x = d_y;
    if(d_z > dL_max_x) dL_max_x = d_z;
    
    d_x = dLL_x[i]*(1+beta*(bias_x[i]))*exp(-beta*bias_x[i]);
    d_y = dLL_y[i]*(1+beta*(bias_y[i]))*exp(-beta*bias_y[i]);    
    d_z = dLL_z[i]*(1+beta*(bias_z[i]))*exp(-beta*bias_z[i]);      
 
  if(dL_max_x > 0.0) {  
    
    meas_x[i] = (int)roundf(100.0*d_x/dL_max_x);
    
  } else {
    
    meas_x[i] = -1;
    
  }
    
  if(dL_max_x > 0.0) {   
  
    meas_y[i] = (int)roundf(100.0*d_y/dL_max_x);

  } else {
    
    meas_y[i] = -1;
    
  }
  
  if(dL_max_x > 0.0) {   
    
    meas_z[i] = (int)roundf(100.0*d_z/dL_max_x);
    
  } else {
    
    meas_z[i] = -1;
    
  }

 dL_max_y = dL_max_x;
 dL_max_z = dL_max_x; 
  
 int diff_time_hist;

 diff_time_hist = 0;

// if(meas_x[i] - meas_x_last[i] != 0 || meas_y[i] - meas_y_last[i] != 0 || meas_z[i] - meas_z_last[i] != 0) diff_time_hist = 1;
 if(stepper%1000 == 0) diff_time_hist = 1;
 
// meas_x_last[i] = meas_x[i];
// meas_y_last[i] = meas_y[i];
// meas_z_last[i] = meas_z[i];  

// delta_time2 = stepper - time_last2;

 if(diff_time_hist == 1) { //&& delta_time2 >= 10) { 
  
//  time_last2 = stepper;

  range_x = -1.0;
  range_y = -1.0;
  range_z = -1.0;  
  
  for(k=1;k<=200;k++) {
    
    if(dL_max_x > 0.0) 
    
    {
    
    W1 = 2.479*(10.0*1000.0/((double)time_step2));
      
    if(W1 > 25.0) W1 = 25.0;
    
    hist_global_x[i][k] -= W1*exp(-pow(d_x/dL_max_x - range_x,2)/0.02)*exp(hist_global_x[i][k]/1000.0);
    hist_global_y[i][k] -= W1*exp(-pow(d_y/dL_max_x - range_y,2)/0.02)*exp(hist_global_y[i][k]/1000.0);    
    hist_global_z[i][k] -= W1*exp(-pow(d_z/dL_max_x - range_z,2)/0.02)*exp(hist_global_z[i][k]/1000.0);    
    
    }
    
    range_x += 0.01;
    range_y += 0.01;    
    range_z += 0.01;
    
  };
  
  bias_xx += bias_x[i];
  bias_yy += bias_y[i];  
  bias_zz += bias_z[i];
  
 };
 
 V_bias += hist_global_x[i][meas_x[i]] + hist_global_y[i][meas_y[i]] + hist_global_z[i][meas_z[i]];
 
};


bias_xx /= ((double)natoms1);
bias_yy /= ((double)natoms1);
bias_zz /= ((double)natoms1);

if(stepper > 1000) {

for(i=1;i<=natoms1;i++) {
  
  if(meas_x[i] > 0 && meas_x[i] <= 200 && meas_y[i] > 0 && meas_y[i] <= 200 && meas_z[i] > 0 && meas_z[i] <= 200 && dL_max_x > 0.0)
  
  { 
  
  d_x = 0.0;
  d_y = 0.0;
  d_z = 0.0;  
    
  d_t = sqrt(pow(diff_L_xc[i],2) + pow(diff_L_yc[i],2) + pow(diff_L_zc[i],2));  
    
 if(d_t > 0.0) { 
  
  d_x = 100.0/dL_max_x*((hist_global_x[i][meas_x[i]] - hist_global_x[i][meas_x[i]-1]));
  d_y = 100.0/dL_max_x*((hist_global_y[i][meas_y[i]] - hist_global_y[i][meas_y[i]-1]));  
  d_z = 100.0/dL_max_x*((hist_global_z[i][meas_z[i]] - hist_global_z[i][meas_z[i]-1]));
  
 }
  
  d_x2 = (f_global[at_id[i]][0]*(1+(bias_x[i]))*exp(-bias_x[i])*(state_global->x[at_id[i]][0] - x_1[i]/tau_1[i]) + dLL_x[i]*(1+(bias_x[i]))*exp(-bias_x[i]));
  d_y2 = (f_global[at_id[i]][1]*(1+(bias_y[i]))*exp(-bias_y[i])*(state_global->x[at_id[i]][1] - y_1[i]/tau_1[i]) + dLL_y[i]*(1+(bias_y[i]))*exp(-bias_y[i]));
  d_z2 = (f_global[at_id[i]][2]*(1+(bias_z[i]))*exp(-bias_z[i])*(state_global->x[at_id[i]][2] - z_1[i]/tau_1[i]) + dLL_z[i]*(1+(bias_z[i]))*exp(-bias_z[i]));
  
  d_t  = sqrt(pow(d_x2,2) + pow(d_y2,2) + pow(d_z2,2));
  
 if(d_t > 0.0) { 
  
    f_global[at_id[i]][0] -= d_x2/d_t*(d_x);
    f_global[at_id[i]][1] -= d_y2/d_t*(d_y);
    f_global[at_id[i]][2] -= d_z2/d_t*(d_z);  
    
   }  
  }

  f_global[at_id[i]][0] *= (1+(beta*bias_x[i]))*exp(-beta*bias_x[i]);
  f_global[at_id[i]][1] *= (1+(beta*bias_y[i]))*exp(-beta*bias_y[i]);
  f_global[at_id[i]][2] *= (1+(beta*bias_z[i]))*exp(-beta*bias_z[i]);	  
  
  x_1[i] += state_global->x[at_id[i]][0];
  y_1[i] += state_global->x[at_id[i]][1];  
  z_1[i] += state_global->x[at_id[i]][2];  
  
}
}
   
l_max_tt   /= (3.0 * (double)natoms1); 
l_max_tt2   = l_max_tt;  

l_max_tt3   = l_max_tt;

l_max_tt    = exp(-1E-3*(enerd->term[F_EPOT] + V_bias - en_last)/(BOLTZ*enerd->term[F_TEMP]));

l_max_tt2   = exp(-1E-3*(enerd->term[F_EPOT])/(BOLTZ*enerd->term[F_TEMP]));

delta_time = stepper - time_last;

  
if(delta_time >= time_step2) {    
  
time_last = stepper;  
  
      if(instance >= num_kmc_states) instance = 1;  
       
	  if(instance <= num_kmc_states) {  
	    
	    for(i=1;i<=natoms1;i++) {
	      
	      states_x[i][instance] = state_global->x[at_id[i]][0];
	      states_y[i][instance] = state_global->x[at_id[i]][1];	      
	      states_z[i][instance] = state_global->x[at_id[i]][2];

              state_vel_x[i][instance] = state_global->v[at_id[i]][0];
              state_vel_y[i][instance] = state_global->v[at_id[i]][1];
              state_vel_z[i][instance] = state_global->v[at_id[i]][2];      
	   
	      time_step[instance] = time_range*(1.0/l_max_tt);
	      
	    };
	    
             rate[instance] = 1.0/(time_range)*l_max_tt;  
     
            number_of_states ++;
	    
        };
        
        instance ++;	
	
        en_last = enerd->term[F_EPOT] + V_bias;        
  
//	time_range = ((double)natoms1/22*10000);
	
	beta     = (rand()%100/100.0);
	
      if(number_of_states >= 2) {  
        
	   recurrence_time = 1;
	   
	   kmc_init  = 1;
	   kmc_init2 = 1;
       
	   stepper_3 = 0;
	   
	    rate_accum[0] = 0.0;
	    sum_rate      = 0.0;
	    
	    for(k=1;k<=num_kmc_states;k++) {
	      
	      rate_accum[k] = rate_accum[k-1] + rate[k];
              sum_rate     += rate[k];	     
	  
	    }
	    
	    select_kmc = 0;
        
            ran = rand()%100/100.0;
	    
	    for(k=1;k<=num_kmc_states;k++) {
	     
	      if( rate_accum[k-1] < sum_rate*ran && sum_rate*ran <= rate_accum[k]) {
		
		     select_kmc = k;
		
	      };
	    };
   
	  contract = 0;
	    
	  ran = rand()%100/100.0;
	    
	  if(ran <= (bias_xx + bias_yy + bias_zz)/3.0) contract = 1;
	    
	  if(select_kmc != 0) {  

	    if(time_step[select_kmc] != 0) time_step2 = time_step[select_kmc];
	    
	   for(i=1;i<=natoms1;i++) {
	     
	      state_global->x[at_id[i]][0] = states_x[i][select_kmc];
	      state_global->x[at_id[i]][1] = states_y[i][select_kmc];	     
	      state_global->x[at_id[i]][2] = states_z[i][select_kmc];
         
              state_global->v[at_id[i]][0] = state_vel_x[i][select_kmc]*2.0*(rand()%100/100.0-0.5);
              state_global->v[at_id[i]][1] = state_vel_y[i][select_kmc]*2.0*(rand()%100/100.0-0.5);
              state_global->v[at_id[i]][2] = state_vel_z[i][select_kmc]*2.0*(rand()%100/100.0-0.5);

            };
	   };
          };
       
	  bNStList = TRUE;
	  bNS      = bNStList;
	  if(ir->implicit_solvent) bBornRadii = TRUE;
	  
	  } else {
	    
	   recurrence_time = 0;	    
	   
	  };	  
	  
	  stepper_3 += 1.0;
	  
       };

     if(stepper > 1000) {  
       
       if(DOMAINDECOMP(cr)) dd_distribute_vec(cr->dd,dd_charge_groups_global(cr->dd),state_global->v,state->v);       
       if(DOMAINDECOMP(cr)) dd_distribute_vec(cr->dd,dd_charge_groups_global(cr->dd),state_global->x,state->x);   
       if(DOMAINDECOMP(cr)) dd_distribute_vec(cr->dd,dd_charge_groups_global(cr->dd),f_global,f);

     }
       
      gmx_bool buff_d = bNStList;

      MPI_Bcast(&buff_d,1,MPI_C_BOOL,0,cr->mpi_comm_mygroup);

      bNStList = buff_d;       
       
      buff_d = bNS;

      MPI_Bcast(&buff_d,1,MPI_C_BOOL,0,cr->mpi_comm_mygroup);

      bNS = buff_d;        
      
      if(ir->implicit_solvent){
      
      gmx_bool buff_d = bBornRadii;

      MPI_Bcast(&buff_d,1,MPI_C_BOOL,0,cr->mpi_comm_mygroup);

      bBornRadii = buff_d;     
      
      }
      
       if(DOMAINDECOMP(cr)) MPI_Barrier(cr->mpi_comm_mygroup);	     
       
        /* ########  END FIRST UPDATE STEP  ############## */
        /* ########  If doing VV, we now have v(dt) ###### */

        /* ################## START TRAJECTORY OUTPUT ################# */

        /* Now we have the energies and forces corresponding to the 
         * coordinates at time t. We must output all of this before
         * the update.
         * for RerunMD t is read from input trajectory
         */
        GMX_MPE_LOG(ev_output_start);
	
        mdof_flags = 0;
        if (do_per_step(step,ir->nstxout)) { mdof_flags |= MDOF_X; }
        if (do_per_step(step,ir->nstvout)) { mdof_flags |= MDOF_V; }
        if (do_per_step(step,ir->nstfout)) { mdof_flags |= MDOF_F; }
        if (do_per_step(step,ir->nstxtcout)) { mdof_flags |= MDOF_XTC; }
        if (bCPT) { mdof_flags |= MDOF_CPT; };

#if defined(GMX_FAHCORE) || defined(GMX_WRITELASTSTEP)
        if (bLastStep)
        {
            /* Enforce writing positions and velocities at end of run */
            mdof_flags |= (MDOF_X | MDOF_V);
        }
#endif
#ifdef GMX_FAHCORE
        if (MASTER(cr))
            fcReportProgress( ir->nsteps, step );

        /* sync bCPT and fc record-keeping */
        if (bCPT && MASTER(cr))
            fcRequestCheckPoint();
#endif
        
        if (mdof_flags != 0)
        {
            wallcycle_start(wcycle,ewcTRAJ);
            if (bCPT)
            {
                if (state->flags & (1<<estLD_RNG))
                {
                    get_stochd_state(upd,state);
                }
                if (MASTER(cr))
                {
                    if (bSumEkinhOld)
                    {
                        state_global->ekinstate.bUpToDate = FALSE;
                    }
                    else
                    {
                        update_ekinstate(&state_global->ekinstate,ekind);
                        state_global->ekinstate.bUpToDate = TRUE;
                    }
                    update_energyhistory(&state_global->enerhist,mdebin);
                }
            }
            write_traj(fplog,cr,outf,mdof_flags,top_global,
                       step,t,state,state_global,f,f_global,&n_xtc,&x_xtc);
            if (bCPT)
            {
                nchkpt++;
                bCPT = FALSE;
            }
            debug_gmx();
            if (bLastStep && step_rel == ir->nsteps &&
                (Flags & MD_CONFOUT) && MASTER(cr) &&
                !bRerunMD && !bFFscan)
            {
                /* x and v have been collected in write_traj,
                 * because a checkpoint file will always be written
                 * at the last step.
                 */
                fprintf(stderr,"\nWriting final coordinates.\n");
                if (ir->ePBC != epbcNONE && !ir->bPeriodicMols &&
                    DOMAINDECOMP(cr))
                {
                    /* Make molecules whole only for confout writing */
                    do_pbc_mtop(fplog,ir->ePBC,state->box,top_global,state_global->x);
                }
                write_sto_conf_mtop(ftp2fn(efSTO,nfile,fnm),
                                    *top_global->name,top_global,
                                    state_global->x,state_global->v,
                                    ir->ePBC,state->box);
                debug_gmx();
            }
            wallcycle_stop(wcycle,ewcTRAJ);
        }
        GMX_MPE_LOG(ev_output_finish);
        
        /* kludge -- virial is lost with restart for NPT control. Must restart */
        if (bStartingFromCpt && bVV) 
        {
            copy_mat(state->svir_prev,shake_vir);
            copy_mat(state->fvir_prev,force_vir);
        }
        /*  ################## END TRAJECTORY OUTPUT ################ */
        
        /* Determine the wallclock run time up till now */
        run_time = gmx_gettime() - (double)runtime->real;

        /* Check whether everything is still allright */    
        if (((int)gmx_get_stop_condition() > handled_stop_condition)
#ifdef GMX_THREADS
            && MASTER(cr)
#endif
            )
        {
            /* this is just make gs.sig compatible with the hack 
               of sending signals around by MPI_Reduce with together with
               other floats */
            if ( gmx_get_stop_condition() == gmx_stop_cond_next_ns )
                gs.sig[eglsSTOPCOND]=1;
            if ( gmx_get_stop_condition() == gmx_stop_cond_next )
                gs.sig[eglsSTOPCOND]=-1;
            /* < 0 means stop at next step, > 0 means stop at next NS step */
            if (fplog)
            {
                fprintf(fplog,
                        "\n\nReceived the %s signal, stopping at the next %sstep\n\n",
                        gmx_get_signal_name(),
                        gs.sig[eglsSTOPCOND]==1 ? "NS " : "");
                fflush(fplog);
            }
            fprintf(stderr,
                    "\n\nReceived the %s signal, stopping at the next %sstep\n\n",
                    gmx_get_signal_name(),
                    gs.sig[eglsSTOPCOND]==1 ? "NS " : "");
            fflush(stderr);
            handled_stop_condition=(int)gmx_get_stop_condition();
        }
        else if (MASTER(cr) && (bNS || ir->nstlist <= 0) &&
                 (max_hours > 0 && run_time > max_hours*60.0*60.0*0.99) &&
                 gs.sig[eglsSTOPCOND] == 0 && gs.set[eglsSTOPCOND] == 0)
        {
            /* Signal to terminate the run */
            gs.sig[eglsSTOPCOND] = 1;
            if (fplog)
            {
                fprintf(fplog,"\nStep %s: Run time exceeded %.3f hours, will terminate the run\n",gmx_step_str(step,sbuf),max_hours*0.99);
            }
            fprintf(stderr, "\nStep %s: Run time exceeded %.3f hours, will terminate the run\n",gmx_step_str(step,sbuf),max_hours*0.99);
        }

        if (bResetCountersHalfMaxH && MASTER(cr) &&
            run_time > max_hours*60.0*60.0*0.495)
        {
            gs.sig[eglsRESETCOUNTERS] = 1;
        }

        if (ir->nstlist == -1 && !bRerunMD)
        {
            /* When bGStatEveryStep=FALSE, global_stat is only called
             * when we check the atom displacements, not at NS steps.
             * This means that also the bonded interaction count check is not
             * performed immediately after NS. Therefore a few MD steps could
             * be performed with missing interactions.
             * But wrong energies are never written to file,
             * since energies are only written after global_stat
             * has been called.
             */
            if (step >= nlh.step_nscheck)
            {
                nlh.nabnsb = natoms_beyond_ns_buffer(ir,fr,&top->cgs,
                                                     nlh.scale_tot,state->x);
            }
            else
            {
                /* This is not necessarily true,
                 * but step_nscheck is determined quite conservatively.
                 */
                nlh.nabnsb = 0;
            }
        }

        /* In parallel we only have to check for checkpointing in steps
         * where we do global communication,
         *  otherwise the other nodes don't know.
         */
        if (MASTER(cr) && ((bGStat || !PAR(cr)) &&
                           cpt_period >= 0 &&
                           (cpt_period == 0 || 
                            run_time >= nchkpt*cpt_period*60.0)) &&
            gs.set[eglsCHKPT] == 0)
        {
            gs.sig[eglsCHKPT] = 1;
        }
  
        if (bIterations)
        {
            gmx_iterate_init(&iterate,bIterations);
        }
    
        /* for iterations, we save these vectors, as we will be redoing the calculations */
        if (bIterations && iterate.bIterate) 
        {
            copy_coupling_state(state,bufstate,ekind,ekind_save,&(ir->opts));
        }
        bFirstIterate = TRUE;
        while (bFirstIterate || (bIterations && iterate.bIterate))
        {
            /* We now restore these vectors to redo the calculation with improved extended variables */    
            if (bIterations) 
            { 
                copy_coupling_state(bufstate,state,ekind_save,ekind,&(ir->opts));
            }

            /* We make the decision to break or not -after- the calculation of Ekin and Pressure,
               so scroll down for that logic */
            
            /* #########   START SECOND UPDATE STEP ################# */
            GMX_MPE_LOG(ev_update_start);
            /* Box is changed in update() when we do pressure coupling,
             * but we should still use the old box for energy corrections and when
             * writing it to the energy file, so it matches the trajectory files for
             * the same timestep above. Make a copy in a separate array.
             */
            copy_mat(state->box,lastbox);

            bOK = TRUE;
            if (!(bRerunMD && !rerun_fr.bV && !bForceUpdate))
            {
                wallcycle_start(wcycle,ewcUPDATE);
                dvdl = 0;
                /* UPDATE PRESSURE VARIABLES IN TROTTER FORMULATION WITH CONSTRAINTS */
                if (bTrotter) 
                {
                    if (bIterations && iterate.bIterate) 
                    {
                        if (bFirstIterate) 
                        {
                            scalevir = 1;
                        }
                        else 
                        {
                            /* we use a new value of scalevir to converge the iterations faster */
                            scalevir = tracevir/trace(shake_vir);
                        }
                        msmul(shake_vir,scalevir,shake_vir); 
                        m_add(force_vir,shake_vir,total_vir);
                        clear_mat(shake_vir);
                    }
                    trotter_update(ir,step,ekind,enerd,state,total_vir,mdatoms,&MassQ,trotter_seq,ettTSEQ3);
                /* We can only do Berendsen coupling after we have summed
                 * the kinetic energy or virial. Since the happens
                 * in global_state after update, we should only do it at
                 * step % nstlist = 1 with bGStatEveryStep=FALSE.
                 */
                }
                else 
                {
                    update_tcouple(fplog,step,ir,state,ekind,wcycle,upd,&MassQ,mdatoms);
                    update_pcouple(fplog,step,ir,state,pcoupl_mu,M,wcycle,
                                   upd,bInitStep);
                }

                if (bVV)
                {
                    /* velocity half-step update */
                    update_coords(fplog,step,ir,mdatoms,state,f,
                                  fr->bTwinRange && bNStList,fr->f_twin,fcd,
                                  ekind,M,wcycle,upd,FALSE,etrtVELOCITY2,
                                  cr,nrnb,constr,&top->idef);
                }

                /* Above, initialize just copies ekinh into ekin,
                 * it doesn't copy position (for VV),
                 * and entire integrator for MD.
                 */
                
                if (ir->eI==eiVVAK) 
                {
                    copy_rvecn(state->x,cbuf,0,state->natoms);
                }
                
                update_coords(fplog,step,ir,mdatoms,state,f,fr->bTwinRange && bNStList,fr->f_twin,fcd,
                              ekind,M,wcycle,upd,bInitStep,etrtPOSITION,cr,nrnb,constr,&top->idef);
                wallcycle_stop(wcycle,ewcUPDATE);

                update_constraints(fplog,step,&dvdl,ir,ekind,mdatoms,state,graph,f,
                                   &top->idef,shake_vir,force_vir,
                                   cr,nrnb,wcycle,upd,constr,
                                   bInitStep,FALSE,bCalcEnerPres,state->veta);  
                
                if (ir->eI==eiVVAK) 
                {
                    /* erase F_EKIN and F_TEMP here? */
                    /* just compute the kinetic energy at the half step to perform a trotter step */
                    compute_globals(fplog,gstat,cr,ir,fr,ekind,state,state_global,mdatoms,nrnb,vcm,
                                    wcycle,enerd,force_vir,shake_vir,total_vir,pres,mu_tot,
                                    constr,NULL,FALSE,lastbox,
                                    top_global,&pcurr,top_global->natoms,&bSumEkinhOld,
                                    cglo_flags | CGLO_TEMPERATURE    
                        );
                    wallcycle_start(wcycle,ewcUPDATE);
                    trotter_update(ir,step,ekind,enerd,state,total_vir,mdatoms,&MassQ,trotter_seq,ettTSEQ4);            
                    /* now we know the scaling, we can compute the positions again again */
                    copy_rvecn(cbuf,state->x,0,state->natoms);

                    update_coords(fplog,step,ir,mdatoms,state,f,fr->bTwinRange && bNStList,fr->f_twin,fcd,
                                  ekind,M,wcycle,upd,bInitStep,etrtPOSITION,cr,nrnb,constr,&top->idef);
                    wallcycle_stop(wcycle,ewcUPDATE);

                    /* do we need an extra constraint here? just need to copy out of state->v to upd->xp? */
                    /* are the small terms in the shake_vir here due
                     * to numerical errors, or are they important
                     * physically? I'm thinking they are just errors, but not completely sure. 
                     * For now, will call without actually constraining, constr=NULL*/
                    update_constraints(fplog,step,&dvdl,ir,ekind,mdatoms,state,graph,f,
                                       &top->idef,tmp_vir,force_vir,
                                       cr,nrnb,wcycle,upd,NULL,
                                       bInitStep,FALSE,bCalcEnerPres,
                                       state->veta);  
                }
                if (!bOK && !bFFscan) 
                {
                    gmx_fatal(FARGS,"Constraint error: Shake, Lincs or Settle could not solve the constrains");
                }
                
                if (fr->bSepDVDL && fplog && do_log) 
                {
                    fprintf(fplog,sepdvdlformat,"Constraint",0.0,dvdl);
                }
                enerd->term[F_DHDL_CON] += dvdl;
            } 
            else if (graph) 
            {
                /* Need to unshift here */
                unshift_self(graph,state->box,state->x);
            }
            
            GMX_BARRIER(cr->mpi_comm_mygroup);
            GMX_MPE_LOG(ev_update_finish);

            if (vsite != NULL) 
            {
                wallcycle_start(wcycle,ewcVSITECONSTR);
                if (graph != NULL) 
                {
                    shift_self(graph,state->box,state->x);
                }
                construct_vsites(fplog,vsite,state->x,nrnb,ir->delta_t,state->v,
                                 top->idef.iparams,top->idef.il,
                                 fr->ePBC,fr->bMolPBC,graph,cr,state->box);
                
                if (graph != NULL) 
                {
                    unshift_self(graph,state->box,state->x);
                }
                wallcycle_stop(wcycle,ewcVSITECONSTR);
            }
            
            /* ############## IF NOT VV, Calculate globals HERE, also iterate constraints ############ */
            if (ir->nstlist == -1 && bFirstIterate)
            {
                gs.sig[eglsNABNSB] = nlh.nabnsb;
            }
            compute_globals(fplog,gstat,cr,ir,fr,ekind,state,state_global,mdatoms,nrnb,vcm,
                            wcycle,enerd,force_vir,shake_vir,total_vir,pres,mu_tot,
                            constr,
                            bFirstIterate ? &gs : NULL, 
                            (step_rel % gs.nstms == 0) && 
                                (multisim_nsteps<0 || (step_rel<multisim_nsteps)),
                            lastbox,
                            top_global,&pcurr,top_global->natoms,&bSumEkinhOld,
                            cglo_flags 
                            | (!EI_VV(ir->eI) ? CGLO_ENERGY : 0) 
                            | (!EI_VV(ir->eI) ? CGLO_TEMPERATURE : 0) 
                            | (!EI_VV(ir->eI) || bRerunMD ? CGLO_PRESSURE : 0) 
                            | (bIterations && iterate.bIterate ? CGLO_ITERATE : 0) 
                            | (bFirstIterate ? CGLO_FIRSTITERATE : 0)
                            | CGLO_CONSTRAINT 
                );
            if (ir->nstlist == -1 && bFirstIterate)
            {
                nlh.nabnsb = gs.set[eglsNABNSB];
                gs.set[eglsNABNSB] = 0;
            }
            /* bIterate is set to keep it from eliminating the old ekin kinetic energy terms */
            /* #############  END CALC EKIN AND PRESSURE ################# */
        
            /* Note: this is OK, but there are some numerical precision issues with using the convergence of
               the virial that should probably be addressed eventually. state->veta has better properies,
               but what we actually need entering the new cycle is the new shake_vir value. Ideally, we could
               generate the new shake_vir, but test the veta value for convergence.  This will take some thought. */

            if (bIterations && 
                done_iterating(cr,fplog,step,&iterate,bFirstIterate,
                               trace(shake_vir),&tracevir)) 
            {
                break;
            }
            bFirstIterate = FALSE;
        }

        update_box(fplog,step,ir,mdatoms,state,graph,f,
                   ir->nstlist==-1 ? &nlh.scale_tot : NULL,pcoupl_mu,nrnb,wcycle,upd,bInitStep,FALSE);
        
        /* ################# END UPDATE STEP 2 ################# */
        /* #### We now have r(t+dt) and v(t+dt/2)  ############# */
    
        /* The coordinates (x) were unshifted in update */
        if (bFFscan && (shellfc==NULL || bConverged))
        {
            if (print_forcefield(fplog,enerd->term,mdatoms->homenr,
                                 f,NULL,xcopy,
                                 &(top_global->mols),mdatoms->massT,pres))
            {
                if (gmx_parallel_env_initialized())
                {
                    gmx_finalize();
                }
                fprintf(stderr,"\n");
                exit(0);
            }
        }
        if (!bGStat)
        {
            /* We will not sum ekinh_old,                                                            
             * so signal that we still have to do it.                                                
             */
            bSumEkinhOld = TRUE;
        }
        
        if (bTCR)
        {
            /* Only do GCT when the relaxation of shells (minimization) has converged,
             * otherwise we might be coupling to bogus energies. 
             * In parallel we must always do this, because the other sims might
             * update the FF.
             */

            /* Since this is called with the new coordinates state->x, I assume
             * we want the new box state->box too. / EL 20040121
             */
            do_coupling(fplog,oenv,nfile,fnm,tcr,t,step,enerd->term,fr,
                        ir,MASTER(cr),
                        mdatoms,&(top->idef),mu_aver,
                        top_global->mols.nr,cr,
                        state->box,total_vir,pres,
                        mu_tot,state->x,f,bConverged);
            debug_gmx();
        }

        /* #########  BEGIN PREPARING EDR OUTPUT  ###########  */
        
        /* sum up the foreign energy and dhdl terms */
        sum_dhdl(enerd,state->lambda,ir);

        /* use the directly determined last velocity, not actually the averaged half steps */
        if (bTrotter && ir->eI==eiVV) 
        {
            enerd->term[F_EKIN] = last_ekin;
        }
        enerd->term[F_ETOT] = enerd->term[F_EPOT] + enerd->term[F_EKIN];
        
        if (bVV)
        {
            enerd->term[F_ECONSERVED] = enerd->term[F_ETOT] + saved_conserved_quantity;
        }
        else 
        {
            enerd->term[F_ECONSERVED] = enerd->term[F_ETOT] + compute_conserved_from_auxiliary(ir,state,&MassQ);
        }
        /* Check for excessively large energies */
        if (bIonize) 
        {
#ifdef GMX_DOUBLE
            real etot_max = 1e200;
#else
            real etot_max = 1e30;
#endif
            if (fabs(enerd->term[F_ETOT]) > etot_max) 
            {
                fprintf(stderr,"Energy too large (%g), giving up\n",
                        enerd->term[F_ETOT]);
            }
        }
        /* #########  END PREPARING EDR OUTPUT  ###########  */
        
        /* Time for performance */
        if (((step % stepout) == 0) || bLastStep) 
        {
            runtime_upd_proc(runtime);
        }
        
        /* Output stuff */
        if (MASTER(cr))
        {
            gmx_bool do_dr,do_or;
            
            if (!(bStartingFromCpt && (EI_VV(ir->eI)))) 
            {
                if (bNstEner)
                {
                    upd_mdebin(mdebin,bDoDHDL, TRUE,
                               t,mdatoms->tmass,enerd,state,lastbox,
                               shake_vir,force_vir,total_vir,pres,
                               ekind,mu_tot,constr);
                }
                else
                {
                    upd_mdebin_step(mdebin);
                }
                
                do_dr  = do_per_step(step,ir->nstdisreout);
                do_or  = do_per_step(step,ir->nstorireout);
                
                print_ebin(outf->fp_ene,do_ene,do_dr,do_or,do_log?fplog:NULL,
                           step,t,
                           eprNORMAL,bCompact,mdebin,fcd,groups,&(ir->opts));
            }
            if (ir->ePull != epullNO)
            {
                pull_print_output(ir->pull,step,t);
            }
            
            if (do_per_step(step,ir->nstlog))
            {
                if(fflush(fplog) != 0)
                {
                    gmx_fatal(FARGS,"Cannot flush logfile - maybe you are out of quota?");
                }
            }
        }


        /* Remaining runtime */
        if (MULTIMASTER(cr) && (do_verbose || gmx_got_usr_signal() ))
        {
            if (shellfc) 
            {
                fprintf(stderr,"\n");
            }
            print_time(stderr,runtime,step,ir,cr);
        }

       double buff = l_max_tt2;

       MPI_Bcast(&buff,1,MPI_DOUBLE,0,cr->mpi_comm_mygroup);

       l_max_tt2 = buff;	        
        
       int   buff2 = recurrence_time;
       
       MPI_Bcast(&buff2,1,MPI_INT,0,cr->mpi_comm_mygroup);       
	  
       recurrence_time = buff2;        
        
        /* Replica exchange */
        bExchanged = FALSE;
        if ((repl_ex_nst > 0) && (step > 0) && !bLastStep && 
        do_per_step(step,repl_ex_nst)) 
    //     recurrence_time == 1)
        {	    
	    
	    bNStList = TRUE;
	    bNS      = TRUE;
	    if(ir->implicit_solvent) bBornRadii = TRUE;
	  
            bExchanged = replica_exchange(fplog,cr,repl_ex,
                                          state_global,enerd->term,
                                          state,step,t);

            if (bExchanged && DOMAINDECOMP(cr)) 
            {
                dd_partition_system(fplog,step,cr,TRUE,1,
                                    state_global,top_global,ir,
                                    state,&f,mdatoms,top,fr,
                                    vsite,shellfc,constr,
                                    nrnb,wcycle,FALSE);
            }
        
        if(bExchanged) {
        
        if(DOMAINDECOMP(cr)){

                 dd_collect_state(cr->dd,state,state_global);

          };

        if(MASTER(cr))    
          {         

         l_max_tt    = exp(-1E-3*(enerd->term[F_EPOT] - en_last)/(BOLTZ*enerd->term[F_TEMP]));
  
         if(instance >= num_kmc_states) instance = 1;  
       
	  if(instance <= num_kmc_states) {  
	    
	    for(i=1;i<=natoms1;i++) {
	      
	      states_x[i][instance-1] = state_global->x[at_id[i]][0];
	      states_y[i][instance-1] = state_global->x[at_id[i]][1];	      
	      states_z[i][instance-1] = state_global->x[at_id[i]][2];

              state_vel_x[i][instance-1] = state_global->v[at_id[i]][0];
              state_vel_y[i][instance-1] = state_global->v[at_id[i]][1];
              state_vel_z[i][instance-1] = state_global->v[at_id[i]][2];      
	   
	      time_step[instance-1] = time_range*(1.0/l_max_tt);
	      
	    };
	    
             rate[instance-1] = 1.0/(time_range)*l_max_tt;  	    
	    
	   }
	  }
	  
	  recurrence_time = 1;
	  
	 }
	}    	    
	      
       if(DOMAINDECOMP(cr)) MPI_Barrier(cr->mpi_comm_mygroup);        
        
        bFirstStep = FALSE;
        bInitStep = FALSE;
        bStartingFromCpt = FALSE;

        /* #######  SET VARIABLES FOR NEXT ITERATION IF THEY STILL NEED IT ###### */
        /* With all integrators, except VV, we need to retain the pressure
         * at the current step for coupling at the next step.
         */
        if ((state->flags & (1<<estPRES_PREV)) &&
            (bGStatEveryStep ||
             (ir->nstpcouple > 0 && step % ir->nstpcouple == 0)))
        {
            /* Store the pressure in t_state for pressure coupling
             * at the next MD step.
             */
            copy_mat(pres,state->pres_prev);
        }
        
        /* #######  END SET VARIABLES FOR NEXT ITERATION ###### */
        
        if (bRerunMD) 
        {
            if (MASTER(cr))
            {
                /* read next frame from input trajectory */
                bNotLastFrame = read_next_frame(oenv,status,&rerun_fr);
            }

            if (PAR(cr))
            {
                rerun_parallel_comm(cr,&rerun_fr,&bNotLastFrame);
            }
        }
        
        if (!bRerunMD || !rerun_fr.bStep)
        {
            /* increase the MD step number */
            step++;
            step_rel++;
        }
        
        cycles = wallcycle_stop(wcycle,ewcSTEP);
        if (DOMAINDECOMP(cr) && wcycle)
        {
            dd_cycles_add(cr->dd,cycles,ddCyclStep);
        }
        
//        if (step_rel == wcycle_get_reset_counters(wcycle) ||
//            gs.set[eglsRESETCOUNTERS] != 0)
//        {
            /* Reset all the counters related to performance over the run */
//            reset_all_counters(fplog,cr,step,&step_rel,ir,wcycle,nrnb,runtime);
//            wcycle_set_reset_counters(wcycle,-1);
            /* Correct max_hours for the elapsed time */
//            max_hours -= run_time/(60.0*60.0);
//            bResetCountersHalfMaxH = FALSE;
//            gs.set[eglsRESETCOUNTERS] = 0;
//        }

    }
    /* End of main MD loop */
    debug_gmx();
    
    /* Stop the time */
    runtime_end(runtime);
    
    if (bRerunMD && MASTER(cr))
    {
        close_trj(status);
    }
    
    if (!(cr->duty & DUTY_PME))
    {
        /* Tell the PME only node to finish */
        gmx_pme_finish(cr);
    }
    
    if (MASTER(cr))
    {
        if (ir->nstcalcenergy > 0 && !bRerunMD) 
        {
            print_ebin(outf->fp_ene,FALSE,FALSE,FALSE,fplog,step,t,
                       eprAVER,FALSE,mdebin,fcd,groups,&(ir->opts));
        }
    }

    done_mdoutf(outf);

    debug_gmx();

    if (ir->nstlist == -1 && nlh.nns > 0 && fplog)
    {
        fprintf(fplog,"Average neighborlist lifetime: %.1f steps, std.dev.: %.1f steps\n",nlh.s1/nlh.nns,sqrt(nlh.s2/nlh.nns - sqr(nlh.s1/nlh.nns)));
        fprintf(fplog,"Average number of atoms that crossed the half buffer length: %.1f\n\n",nlh.ab/nlh.nns);
    }
    
    if (shellfc && fplog)
    {
        fprintf(fplog,"Fraction of iterations that converged:           %.2f %%\n",
                (nconverged*100.0)/step_rel);
        fprintf(fplog,"Average number of force evaluations per MD step: %.2f\n\n",
                tcount/step_rel);
    }
    
    if (repl_ex_nst > 0 && MASTER(cr))
    {
        print_replica_exchange_statistics(fplog,repl_ex);
    }
    
    runtime->nsteps_done = step_rel;
    
    return 0;
}

int random_number(float *x)

{

/*   int seed;
  
   seed = time(NULL);
   srand(seed);  
    fscanf(fp,"%lg\n",&W_factor);    
      
   srand( (unsigned)time( NULL ) ); */
   *x = (float) rand()  / (float) RAND_MAX;

   if(*x <= 0.09) *x = *x * 10;

   if(*x <= 0.009) *x = *x * 100;

   if(*x <= 0.0009) *x = *x * 1000;

   if(*x <= 0.00009) *x = *x * 10000;

   if(*x <= 0.000009) *x = *x * 100000;

   if(*x <= 0.0000009) *x = *x * 1000000;

   return 1;

}

