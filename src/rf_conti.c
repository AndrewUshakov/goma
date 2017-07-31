/************************************************************************ *
* Goma - Multiphysics finite element software                             *
* Sandia National Laboratories                                            *
*                                                                         *
* Copyright (c) 2014 Sandia Corporation.                                  *
*                                                                         *
* Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,  *
* the U.S. Government retains certain rights in this software.            *
*                                                                         *
* This software is distributed under the GNU General Public License.      *
\************************************************************************/
 
		first order continuation */

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>

#define _RF_CONTI_C
#include "goma.h"

int w;

#include "sl_util.h"		/* defines sl_init() */
#include "sl_auxutil.h"

#ifdef HAVE_FRONT
extern int mf_setup
PROTO((int *,			/* nelem_glob */
       int *,			/* neqn_glob */
       int *,			/* mxdofel */
       int *,			/* nfullsum */
       int *,			/* symflag */
       int *,			/* nell_order */
       int *,			/* el_proc_assign */
       int *,			/* level */
       int *,			/* nopdof */
       int *,			/* loc_dof */
       int *,			/* constraint */
       const char *,		/* cname */
       int *));			/* allocated */
#endif

/*

   ZERO AND FIRST ORDER CONTINUATION

   BASED ON solve_problem() IN rf_solve.c

   BY IAN GATES & ROBERT SECOR

   2/98 - 11/99

*/

void
continue_problem (Comm_Ex *cx,	/* array of communications structures */
		  Exo_DB  *exo, /* ptr to the finite element mesh database */
		  Dpi     *dpi) /* distributed processing information */
{
  int    *ija=NULL;		/* column pointer array                         */
  double *a=NULL;		/* nonzero array                                */
  double *a_old=NULL;		/* nonzero array                                */
  double *x=NULL;		/* solution vector                              */

  int     iAC;			/* COUNTER                                      */
  double *x_AC = NULL;		/* SOLUTION VECTOR OF EXTRA UNKNOWNS            */
  double *x_AC_old=NULL;	/* old SOLUTION VECTOR OF EXTRA UNKNOWNS        */
 
  int    *ija_attic=NULL;	/* storage for external dofs                    */

  int eb_indx, ev_indx;

  /* 
   * variables for path traversal 
   */
  double *x_old=NULL;		/* old solution vector                          */
  double *x_older=NULL;		/* older solution vector                        */
  double *x_oldest=NULL;	/* oldest solution vector saved                 */
  double *xdot=NULL;		/* current path derivative of soln              */
  double *xdot_old=NULL;
  double *x_update=NULL;


  double *x_sens=NULL;		/* solution sensitivity */
  double *x_sens_temp=NULL;	/* MMH thinks we need another one, so
				 * that when the solution is updated
				 * on a failure, it doesn't use the
				 * last computed x_sens b/c that might
				 * be crappy.  We should use the last
				 * known good one...  I haven't done
				 * the same thing with x_sens_p.
				 */
  double **x_sens_p=NULL;	/* solution sensitivity for parameters */
  int num_pvector=0;		/*  number of solution sensitivity vectors   */

  struct Aztec_Linear_Solver_System *ams[NUM_ALSS]={NULL, NULL}; 
                 /* sl_util_structs.h */

  double *resid_vector=NULL;	/* residual */
  double *resid_vector_sens=NULL;/* residual sensitivity */

  double *scale=NULL;		/* scale vector for modified newton */

  int 	 *node_to_fill = NULL;	

  int		n;		/* total number of path steps attempted */
  int		ni;		/* total number of nonlinear solves */
  int		nt;		/* total number of successful path steps */
  int		path_step_reform; /* counter for jacobian reformation stride */
  int		converged;	/* success or failure of Newton iteration */
  int		success_ds;	/* success or failure of path step */

  int           i, nprint=0, num_total_nodes;

  int           numProcUnknowns;
  int           const_delta_s, step_print;
  double        path_print, i_print;
  double	path,		/* Current value (should have solution here) */
                path1;		/* New value (would like to get solution here) */
  double	delta_s, delta_s_new, delta_s_old, delta_s_older, delta_s_oldest;
  double        delta_t;
  double	theta=0.0;
  double        damp;
  double        eps;
  double        lambda, lambdaEnd;

  /* 
   * ALC management variables
   */
  int  aldALC,			/* direction of continuation, == -1 =>
				   beginning value is greater than ending value. */
       alqALC;			/* is -1 when we're on our last step. */

  /*
   * Other local variables 
   */
  int	        error, err, is_steady_state, inewton;
  int 		*gindex = NULL, gsize;
  int		*p_gsize=NULL;
  double	*gvec=NULL;
  double        ***gvec_elem=NULL;
  double	err_dbl;
  FILE          *cl_aux=NULL, *file=NULL;
  
  struct Results_Description  *rd=NULL;
  
  int		tnv;		/* total number of nodal variables and kinds */
  int		tev;		/* total number of elem variables and kinds */
  int		tnv_post;	/* total number of nodal variables and kinds 
				   for post processing */
  int		tev_post;	/* total number of elem variables and kinds 
				   for post processing */

  int max_unk_elem, one, three; /* variables used as mf_setup arguments*/

  unsigned int matrix_systems_mask;

  static const char yo[]="continue_problem"; 

  /*
   * 		BEGIN EXECUTION
   */
#ifdef DEBUG
  fprintf(stderr, "%s() begins...\n", yo);
#endif

  is_steady_state = TRUE;

  p_gsize = &gsize;
  
  /* 
   * set aside space for gather global vectors to print to exoII file
   * note: this is temporary
   *
   * For 2D prototype problem:  allocate space for T, dx, dy arrays
   */
  if( strlen( Soln_OutFile)  )
    {
      file = fopen(Soln_OutFile, "w");
      if (file == NULL) {
	DPRINTF(stderr, "%s:  opening soln file for writing\n", yo);
        EH(-1, "\t");
      }
    }
#ifdef PARALLEL
  check_parallel_error("Soln output file error");
#endif
  
  /*
   * Some preliminaries to help setup EXODUS II database output.
   */
#ifdef DEBUG
  fprintf(stderr, "cnt_nodal_vars() begins...\n");
#endif

  /*  
   * tnv_post is calculated in load_nodal_tkn
   * tev_post is calculated in load_elem_tkn
   */
  tnv = cnt_nodal_vars();
  tev = cnt_elem_vars();
  
#ifdef DEBUG
  fprintf(stderr, "Found %d total primitive nodal variables to output.\n", tnv);
  fprintf(stderr, "Found %d total primitive elem variables to output.\n", tev);
#endif
  
  if (tnv < 0)
    {
      DPRINTF(stderr, "%s:\tbad tnv.\n", yo);
      EH(-1, "\t");
    }
  
  rd = (struct Results_Description *) 
    smalloc(sizeof(struct Results_Description));

  if (rd == NULL) 
    EH(-1, "Could not grab Results Description.");

  (void) memset((void *) rd, 0, sizeof(struct Results_Description));
  
  rd->nev = 0;			/* number element variables in results */
  rd->ngv = 0;			/* number global variables in results */
  rd->nhv = 0;			/* number history variables in results */

  rd->ngv = 5;			/* number global variables in results 
				   see load_global_var_info for names*/
  error = load_global_var_info(rd, 0, "CONV");
  error = load_global_var_info(rd, 1, "NEWT_IT");
  error = load_global_var_info(rd, 2, "MAX_IT");
  error = load_global_var_info(rd, 3, "CONVRATE");
  error = load_global_var_info(rd, 4, "MESH_VOLUME");

  /* load nodal types, kinds, names */
  error = load_nodal_tkn(rd, 
                         &tnv, 
                         &tnv_post); 
  
  if (error)
    {
      DPRINTF(stderr, "%s:  problem with load_nodal_tkn()\n", yo);
      EH(-1,"\t");
    }

  /* load elem types, names */
  error = load_elem_tkn(rd,
                        tev, 
                        &tev_post); 
  
  if (error)
    {
      DPRINTF(stderr, "%s:  problem with load_elem_tkn()\n", yo);
      EH(-1,"\t");
    }
#ifdef PARALLEL
  check_parallel_error("Results file error");
#endif

  /* 
   * Write out the names of the nodal variables that we will be sending to
   * the EXODUS II output file later.
   */
#ifdef DEBUG
  fprintf(stderr, "wr_result_prelim() starts...\n", tnv);
#endif

  gvec_elem = (double ***) smalloc ( (exo->num_elem_blocks)*sizeof(double **));
  for (i = 0; i < exo->num_elem_blocks; i++)
    gvec_elem[i] = (double **) smalloc ( (tev + tev_post)*sizeof(double *));

  wr_result_prelim_exo(rd, 
                       exo, 
                       ExoFileOut,
                       gvec_elem );

#ifdef DEBUG
  fprintf(stderr, "P_%d: wr_result_prelim_exo() ends...\n", ProcID, tnv);
#endif

  /* 
   * This gvec workhorse transports output variables as nodal based vectors
   * that are gather from the solution vector. Note: it is NOT a global
   * vector at all and only carries this processor's nodal variables to
   * the exodus database.
   */
  asdv(&gvec, Num_Node);

  /*
   * Allocate space and manipulate for all the nodes that this processor
   * is aware of...
   */
  num_total_nodes = dpi->num_universe_nodes;

  numProcUnknowns = NumUnknowns[pg->imtrx] + NumExtUnknowns[pg->imtrx];

  /* allocate memory for Volume Constraint Jacobian */
  if ( nAC > 0)
    for(iAC=0;iAC<nAC;iAC++)
      augc[iAC].d_evol_dx = (double*) malloc(numProcUnknowns*sizeof(double));
  
  asdv(&resid_vector, numProcUnknowns);
  asdv(&resid_vector_sens, numProcUnknowns);
  asdv(&scale, numProcUnknowns);

  for (i = 0; i < NUM_ALSS; i++) 
    {
      ams[i] = (struct Aztec_Linear_Solver_System *)
	array_alloc(1, 1, sizeof(struct Aztec_Linear_Solver_System )); 
    }

#ifdef TRILINOS
#ifdef MPI
  AZ_set_proc_config( ams[0]->proc_config, MPI_COMM_WORLD );
#ifndef COUPLED_FILL
  if( Explicit_Fill ) AZ_set_proc_config( ams[1]->proc_config, MPI_COMM_WORLD );
#endif
#else
  AZ_set_proc_config( ams[0]->proc_config, 0 );
#ifndef COUPLED_FILL
  if( Explicit_Fill ) AZ_set_proc_config( ams[1]->proc_config, 0 );
#endif
#endif
#else
#ifdef AZTEC_2_1
#ifdef MPI
  AZ_set_proc_config( ams[0]->proc_config, MPI_COMM_WORLD );'
#ifndef COUPLED_FILL
  if( Explicit_Fill ) AZ_set_proc_config( ams[1]->proc_config, MPI_COMM_WORLD );
#endif
#else
  AZ_set_proc_config( ams[0]->proc_config, 0 );
#ifndef COUPLED_FILL
  if( Explicit_Fill ) AZ_set_proc_config( ams[1]->proc_config, 0 );
#endif
#endif
#endif
#endif

  /* 
   * allocate space for and initialize solution arrays 
   */
  asdv(&x,        numProcUnknowns);
  asdv(&x_old,    numProcUnknowns);
  asdv(&x_older,  numProcUnknowns);
  asdv(&x_oldest, numProcUnknowns);
  asdv(&xdot,     numProcUnknowns);
  asdv(&xdot_old, numProcUnknowns);
  asdv(&x_update, numProcUnknowns);
  
  asdv(&x_sens,   numProcUnknowns);
  asdv(&x_sens_temp,   numProcUnknowns);
  
  /*
   * FRIENDLY COMMAND LINE EQUIV
   */
  if( ProcID == 0 )
   {
      cl_aux = fopen("goma-cl.txt", "w+");

      fprintf(cl_aux, "goma -a -i input ");
      fprintf(cl_aux, "-cb %10.6e ", cont->BegParameterValue);
      fprintf(cl_aux, "-ce %10.6e ", cont->EndParameterValue);
      fprintf(cl_aux, "-cd %10.6e ", cont->Delta_s0);
      fprintf(cl_aux, "-cn %d ", cont->MaxPathSteps);
      fprintf(cl_aux, "-cm %d ", Continuation);
      fprintf(cl_aux, "-ct %d ", cont->upType);

      switch (cont->upType)
        {
        case 1:			/* BC TYPE */
          fprintf(cl_aux, "-c_bc %d ", cont->upBCID);
          fprintf(cl_aux, "-c_df %d ", cont->upDFID);
          break;
        case 2:			/* MAT TYPE */
          fprintf(cl_aux, "-c_mn %d ", cont->upMTID+1);
          fprintf(cl_aux, "-c_mp %d ", cont->upMPID);
          break;
        default:
          fprintf(stderr, "%s: Bad cont->upType, %d\n", yo, cont->upType);
          EH(-1,"Bad cont->upType");
          break;			/* duh */
        }

      fprintf(cl_aux, "\n");

      fclose(cl_aux);
   }
#ifdef PARALLEL
  check_parallel_error("Continuation setup error");
#endif
  /*
   * FIRST ORDER CONTINUATION 
   */
  lambda       = cont->BegParameterValue;
  lambdaEnd    = cont->EndParameterValue;
  
  if (lambdaEnd > lambda)
    aldALC = +1;
  else
    aldALC = -1;

  Delta_s0     = cont->Delta_s0;
  Delta_s_min  = cont->Delta_s_min;
  Delta_s_max  = cont->Delta_s_max;
  MaxPathSteps = cont->MaxPathSteps;
  PathMax      = cont->PathMax;
  eps          = cont->eps;
  
  if (Delta_s0 < 0.0 )
    {
      Delta_s0 = -Delta_s0;
      const_delta_s = 1;
    } 
  else 
    const_delta_s = 0;
  
  damp = 1.0;

  path = path1 = lambda;

  if (Debug_Flag && ProcID == 0)
    {
      fprintf(stderr,"MaxPathSteps: %d \tlambdaEnd: %f\n", MaxPathSteps, lambdaEnd);
      fprintf(stderr,"continuation in progress\n");
    }

  nprint = 0;

  if (Delta_s0 > Delta_s_max) 
    Delta_s0 = Delta_s_max;

  delta_s = delta_s_old = delta_s_older = Delta_s0;
      
  delta_t = 0.0;

  /* Call prefront (or mf_setup) if necessary */
  if (Linear_Solver == FRONT)
    {
	  
      /* NB. this is not a fortran interface but benner treats all equally*/
      /* Also got to define these because it wants pointers to these numbers */
      max_unk_elem = (MAX_PROB_VAR + MAX_CONC)*MDE;

      one = 1;
      three = 3;

      /* NOTE: We need a overall flag in the vn_glob struct that tells whether FULL_DG
	 is on anywhere in domain.  This assumes only one material.  See sl_front_setup for test.
	 that test needs to be in the input parser.  */
      if(vn_glob[0]->dg_J_model == FULL_DG) 
	max_unk_elem = (MAX_PROB_VAR + MAX_CONC)*MDE + 4*vn_glob[0]->modes*4*MDE;

#ifdef PARALLEL
  if (Num_Proc > 1) EH(-1, "Whoa.  No front allowed with nproc>1");  
  check_parallel_error("Front solver not allowed with nprocs>1");
#endif
	  
#ifdef HAVE_FRONT  
       err = mf_setup(&exo->num_elems, 
		     &NumUnknowns[pg->imtrx], 
		     &max_unk_elem, 
		     &three,
		     &one,
		     exo->elem_order_map,
		     fs->el_proc_assign,
		     fs->level,
		     fs->nopdof,
		     fs->ncn,
		     fs->constraint,
		     front_scratch_directory,
		     &fs->ntra); 
      EH(err,"problems in frontal setup ");

#else
      EH(-1,"Don't have frontal solver compiled and linked in");
#endif
    }


  /*
   *  if compute parameter sensitivities, allocate space for solution
   *  sensitivity vectors
   */

        for(i=0;i<nn_post_fluxes_sens;i++)     
	  {
	    num_pvector=MAX(num_pvector,pp_fluxes_sens[i]->vector_id);
	  }
        for(i=0;i<nn_post_data_sens;i++)        
	  {
	    num_pvector=MAX(num_pvector,pp_data_sens[i]->vector_id);
	  }

  if((nn_post_fluxes_sens + nn_post_data_sens) > 0)
    {
      num_pvector++;
      num_pvector = MAX(num_pvector,2);
         x_sens_p = Dmatrix_birth(num_pvector,numProcUnknowns);
    }
  else
    x_sens_p = NULL;

  if (nAC > 0)
    {
      asdv(&x_AC, nAC);
      asdv(&x_AC_old, nAC);
    }

  /*
   * ADJUST NATURAL PARAMETER
   */
  update_parameterC(path1, x, xdot, delta_s, 
		    cx, exo, dpi);


  /* Allocate sparse matrix */
  if( strcmp( Matrix_Format, "msr" ) == 0)
    {
      log_msg("alloc_MSR_sparse_arrays...");
      alloc_MSR_sparse_arrays(&ija, 
			      &a, 
			      &a_old, 
			      0, 
			      node_to_fill, 
			      exo, 
			      dpi);
      /*
       * An attic to store external dofs column names is needed when
       * running in parallel.
       */
      alloc_extern_ija_buffer(num_universe_dofs[pg->imtrx], 
			      num_internal_dofs[pg->imtrx] + num_boundary_dofs[pg->imtrx], 
			      ija, &ija_attic);
      /*
       * Any necessary one time initialization of the linear
       * solver package (Aztec).
       */
      ams[JAC]->bindx   = ija;
      ams[JAC]->val     = a;
      ams[JAC]->belfry  = ija_attic;
      ams[JAC]->val_old = a_old;
	  
      /*
       * These point to nowhere since we're using MSR instead of VBR
       * format.
       */
      ams[JAC]->indx  = NULL;
      ams[JAC]->bpntr = NULL;
      ams[JAC]->rpntr = NULL;
      ams[JAC]->cpntr = NULL;
      ams[JAC]->npn      = dpi->num_internal_nodes + dpi->num_boundary_nodes;
      ams[JAC]->npn_plus = dpi->num_internal_nodes + dpi->num_boundary_nodes + dpi->num_external_nodes;

      ams[JAC]->npu      = num_internal_dofs[pg->imtrx] + num_boundary_dofs[pg->imtrx];
      ams[JAC]->npu_plus = num_universe_dofs[pg->imtrx];

      ams[JAC]->nnz = ija[num_internal_dofs[pg->imtrx] + num_boundary_dofs[pg->imtrx]] - 1;
      ams[JAC]->nnz_plus = ija[num_universe_dofs[pg->imtrx]];
    }
  else if(  strcmp( Matrix_Format, "vbr" ) == 0)
    {
      log_msg("alloc_VBR_sparse_arrays...");
      alloc_VBR_sparse_arrays (ams[JAC],
			       exo,
			       dpi);
      ija_attic = NULL;
      ams[JAC]->belfry  = ija_attic;

      a = ams[JAC]->val;
      if( !save_old_A ) a_old = ams[JAC]->val_old = NULL;
    }
  else if ( strcmp( Matrix_Format, "front") == 0 )
    {
      /* Don't allocate any sparse matrix space when using front */
      ams[JAC]->bindx   = NULL;
      ams[JAC]->val     = NULL;
      ams[JAC]->belfry  = NULL;
      ams[JAC]->val_old = NULL;
      ams[JAC]->indx  = NULL;
      ams[JAC]->bpntr = NULL;
      ams[JAC]->rpntr = NULL;
      ams[JAC]->cpntr = NULL;

    }
  else
    EH(-1,"Attempted to allocate unknown sparse matrix format");

  init_vec(x, cx, exo, dpi, x_AC, nAC);

  /*  if read ACs, update data floats */
  if (nAC > 0)
    if(augc[0].iread == 1)
	{
		for(iAC=0 ; iAC<nAC ; iAC++)
			{ update_parameterAC(iAC,x_AC, cx, exo, dpi);}
	}

  vzero(numProcUnknowns, &x_sens[0]);
  vzero(numProcUnknowns, &x_sens_temp[0]);

  /* 
   * set boundary conditions on the initial conditions 
   */
  find_and_set_Dirichlet(x, xdot, exo, dpi);

  exchange_dof(cx, dpi, x);

  dcopy1(numProcUnknowns,x,x_old);
  dcopy1(numProcUnknowns,x_old,x_older);
  dcopy1(numProcUnknowns,x_older,x_oldest);

  if(nAC > 0)
    dcopy1(nAC,x_AC, x_AC_old);

  /* 
   * initialize the counters for when to print out data 
   */
  path_print = path1;
  step_print = 1;
      
  matrix_systems_mask = 1;

  log_msg("sl_init()...");
  sl_init(matrix_systems_mask, ams, exo, dpi, cx, imtrx);

  /*
  * Make sure the solver was properly initialized on all processors.
  */
#ifdef PARALLEL
  check_parallel_error("Solver initialization problems");
#endif

#ifdef AZTEC_2
  ams[JAC]->options[AZ_keep_info] = 1;
#endif
  /* 
   * set the number of successful path steps to zero 
   */
  nt = 0;   

  /* 
   * LOOP THROUGH PARAMETER UNTIL MAX NUMBER 
   * OF STEPS SURPASSED
   */
  for(n = 0; n < MaxPathSteps; n++)
    {
      alqALC = 1;

      switch (aldALC)
	{
	case -1:			/* REDUCING PARAMETER DIRECTION */
	  if (path1 <= lambdaEnd)
	    { 
	      DPRINTF(stderr,"\n\t ******** LAST PATH STEP!\n");
	      alqALC = -1;
	      path1 = lambdaEnd;
	      delta_s = path-path1;
	    } 
	  break;
	case +1:			/* RISING PARAMETER DIRECTION */
	  if (path1 >= lambdaEnd)
	    { 
	      DPRINTF(stderr,"\n\t ******** LAST PATH STEP!\n");
	      alqALC = -1;
	      path1 = lambdaEnd;
	      delta_s = path1-path;
	    } 
	  break;
	default:
	  DPRINTF(stderr, "%s: Bad aldALC, %d\n", yo, aldALC);
          EH(-1,"\t");
	  break;		/* duh */
	}
#ifdef PARALLEL
  check_parallel_error("Bad aldALC");
#endif
	  
      /*
       * ADJUST NATURAL PARAMETER
       */
      update_parameterC(path1, x, xdot, delta_s, 
			cx, exo, dpi);

      /*
       * IF STEP CHANGED, REDO FIRST ORDER PREDICTION
       */
      if(alqALC == -1)
	{
	  dcopy1(NumUnknowns[pg->imtrx],x_old,x);

	  switch (Continuation)
	    {
	    case ALC_ZEROTH:
	      break;
	    case  ALC_FIRST:
	      switch (aldALC)
		{
		case -1:
		  v1add(NumUnknowns[pg->imtrx], &x[0], -delta_s, &x_sens[0]);
		  break;
		case +1:
		  v1add(NumUnknowns[pg->imtrx], &x[0], +delta_s, &x_sens[0]);
		  break;
		default:
		  DPRINTF(stderr, "%s: Bad aldALC, %d\n", yo, aldALC);
                  EH(-1,"\t");
		  break;	/* duh */
		}
	      break;
	    default:
	      DPRINTF(stderr, "%s: Bad Continuation, %d\n", yo, Continuation);
              EH(-1,"\t");
	      break;		/* duh */
	    }
	}
#ifdef PARALLEL
  check_parallel_error("Bad Continuation");
#endif

      find_and_set_Dirichlet (x, xdot, exo, dpi); 

      exchange_dof(cx, dpi, x);

      if (ProcID == 0)
	{
	  fprintf(stderr, "\n\t----------------------------------");
	  switch (Continuation)
	    {
	    case ALC_ZEROTH:
	      DPRINTF(stderr, "\n\tZero Order Continuation:");
	      break;
	    case  ALC_FIRST:
	      DPRINTF(stderr, "\n\tFirst Order Continuation:");
	      break;
	    default:
	      DPRINTF(stderr, "%s: Bad Continuation, %d\n", yo, Continuation);
              EH(-1,"\t");
	      break;		/* duh */
	    }
	  DPRINTF(stderr, "\n\tStep number: %4d of %4d (max)", n+1, MaxPathSteps);
	  DPRINTF(stderr, "\n\tAttempting solution at:");
	  switch (cont->upType)
	    {
	    case 1:		/* BC */
	      DPRINTF(stderr, "\n\tBCID=%3d DFID=%5d", cont->upBCID, cont->upDFID);
	      break;
	    case 2:		/* MT */
	      DPRINTF(stderr, "\n\tMTID=%3d MPID=%5d", cont->upMTID, cont->upMPID);
	      break;
	    default:
	      DPRINTF(stderr, "%s: Bad cont->upType, %d\n", yo, cont->upType);
              EH(-1,"\t");
	      break;		/* duh */
	    }
	  DPRINTF(stderr, " Parameter= % 10.6e delta_s= %10.6e", path1, delta_s);
	}
#ifdef PARALLEL
  check_parallel_error("Bad cont->upType");
#endif
	
      ni = 0;
      do {
	
#ifdef DEBUG
	DPRINTF(stderr, "%s: starting solve_nonlinear_problem\n", yo);
#endif
	err = solve_nonlinear_problem(ams[JAC], 
				      x, 
				      delta_t, 
				      theta,
				      x_old,
				      x_older, 
				      xdot,
				      xdot_old,
				      resid_vector, 
				      x_update,
				      scale, 
				      &converged, 
				      &nprint, 
				      tev, 
				      tev_post,
				      NULL,
				      rd,
				      gindex,
				      p_gsize,
				      gvec, 
				      gvec_elem, 
				      path1,
				      exo, 
				      dpi, 
				      cx, 
				      0, 
				      &path_step_reform,
				      is_steady_state,
				      x_AC, 
				      path1, 
				      resid_vector_sens, 
				      x_sens_temp,
				      x_sens_p);
	  
#ifdef DEBUG
	fprintf(stderr, "%s: returned from solve_nonlinear_problem\n", yo);
#endif

	if (err == -1)
	  converged = 0;
	inewton = err;
	if(converged)
	  {
	    if ( Write_Intermediate_Solutions == 0 ) {
#ifdef DEBUG
	      DPRINTF(stderr, "%s: write_solution call WIS\n", yo);
#endif
	      error = write_solution(ExoFileOut, resid_vector, x, x_sens_p,
				     x_old, xdot, tev, tev_post, NULL, rd, gindex,
				     p_gsize, gvec, gvec_elem, &nprint, 
				     delta_s, theta,  path1, exo, dpi);
#ifdef DEBUG
	      fprintf(stderr, "%s: write_solution end call WIS\n", yo);
#endif
	      EH(error, "error writing exodusII file.");
	    }
#ifdef PARALLEL
              check_parallel_error("Error writing exodusII file");
#endif

	    /*
	     * PRINT OUT VALUES OF EXTRA UNKNOWNS
	     * FROM AUGMENTING CONDITIONS
	     */
	    if (nAC > 0)
	      {
		DPRINTF(stderr, "\n------------------------------\n");
		DPRINTF(stderr, "Augmenting Conditions:    %4d\n", nAC);
		DPRINTF(stderr, "Number of extra unknowns: %4d\n\n", nAC);

		for (iAC = 0; iAC < nAC; iAC++)
                 {
		  if (augc[iAC].Type == AC_USERBC)
                   {
		    DPRINTF(stderr, "\tBC[%4d] DF[%4d] = %10.6e\n",
			    augc[iAC].BCID, augc[iAC].DFID, x_AC[iAC]);
                   }
		  else if (augc[iAC].Type == AC_USERMAT)
                   {
		    DPRINTF(stderr, "\n New MT[%4d] MP[%4d] = %10.6e\n",
			    augc[iAC].MTID, augc[iAC].MPID, x_AC[iAC]);
                   }
                  else if(augc[iAC].Type == AC_VOLUME)
                   {
                    DPRINTF(stderr, "\tMT[%4d] VC[%4d]=%10.6e Param=%10.6e\n",
                            augc[iAC].MTID, augc[iAC].VOLID, augc[iAC].evol,
                            x_AC[iAC]);
                   }
                  else if(augc[iAC].Type == AC_FLUX)
                   {
                    DPRINTF(stderr, "\tBC[%4d] DF[%4d]=%10.6e\n",
                            augc[iAC].BCID, augc[iAC].DFID, x_AC[iAC]);
                   }
                 }
	      }

	    /*
	     * INTEGRATE FLUXES, FORCES
	     */
	    for (i = 0; i < nn_post_fluxes; i++)
	      err_dbl = evaluate_flux (exo, dpi, 
                                       pp_fluxes[i]->ss_id,
				       pp_fluxes[i]->flux_type ,
                                       pp_fluxes[i]->flux_type_name ,
				       pp_fluxes[i]->blk_id ,
				       pp_fluxes[i]->species_number,
				       pp_fluxes[i]->flux_filenm,
                                       pp_fluxes[i]->profile_flag,
				       x,xdot,NULL, delta_s,path1,1);

	    /*
	     * COMPUTE FLUX, FORCE SENSITIVITIES
	     */
	    for (i = 0; i < nn_post_fluxes_sens; i++)
	      err_dbl = evaluate_flux_sens (exo, dpi,
                                            pp_fluxes_sens[i]->ss_id,
					    pp_fluxes_sens[i]->flux_type ,
                                            pp_fluxes_sens[i]->flux_type_name ,
					    pp_fluxes_sens[i]->blk_id ,
					    pp_fluxes_sens[i]->species_number,
					    pp_fluxes_sens[i]->sens_type,
					    pp_fluxes_sens[i]->sens_id,
					    pp_fluxes_sens[i]->sens_flt,
					    pp_fluxes_sens[i]->vector_id,
					    pp_fluxes_sens[i]->flux_filenm,
                                            pp_fluxes_sens[i]->profile_flag,
					    x,xdot,x_sens_p,delta_s,path1,1);

	  }   /*  end of if converged block  */


	/*
	 * INCREMENT COUNTER
	 */
	ni++;

	/*
	 * DID IT CONVERGE ? 
	 * IF NOT, REDUCE STEP SIZE AND TRY AGAIN
	 */
	if (!converged)
	  {
	    if (ni > 5)
	      {
		puts("                                     ");
		puts(" ************************************");
		puts(" W: Did not converge in Newton steps.");
		puts("    Find better initial guess.       ");
		puts(" ************************************"); 
                EH(-1,"\t");
	      }

	    /*
	     * ADJUST STEP SIZE
	     */
	    DPRINTF(stderr, "\n\tFailed to converge:\n");

	    delta_s *= 0.5;

	    switch (aldALC)
	      {
	      case -1: 
		path1 = path - delta_s;
		break;
	      case +1: 
		path1 = path + delta_s;
		break;
	      default:
		DPRINTF(stderr, "%s: Bad aldALC, %d\n", yo, aldALC);
                EH(-1,"\t");
		break;		/* duh */
	      }
#ifdef PARALLEL
              check_parallel_error("Bad aldALC");
#endif

	    /*
	     * RESET
	     */
	    alqALC = 1;		/* If necessary, don't call this the last step... */

	    DPRINTF(stderr, "\n\tDecreasing step-length to %10.6e.\n", delta_s);

	    if (delta_s < Delta_s_min)
	      {
		puts("\n X: C step-length reduced below minimum.");
		puts("\n    Program terminated.\n");
                EH(-1,"\t");
	      } 
#ifdef PARALLEL
              check_parallel_error("\t");
#endif

	    /*
	     * ADJUST NATURAL PARAMETER
	     */
	    dcopy1(numProcUnknowns, x_old, x);
	    update_parameterC(path1, x, xdot, delta_s, 
			      cx, exo, dpi);

	    /*
	     * GET ZERO OR FIRST ORDER PREDICTION
	     */
	    switch (Continuation)
	      {
	      case ALC_ZEROTH:
		break;
	      case  ALC_FIRST:
		switch (aldALC)
		  {
		  case -1: 
		    v1add(numProcUnknowns, &x[0], -delta_s, &x_sens[0]);
		    break;
		  case +1: 
		    v1add(numProcUnknowns, &x[0], +delta_s, &x_sens[0]);
		    break;
		  default:
		    DPRINTF(stderr, "%s: Bad aldALC, %d\n", yo, aldALC);
                    EH(-1,"\t");
		    break;		/* duh */
		  }
		break;
	      default:
		DPRINTF(stderr, "%s: Bad Continuation, %d\n", yo, Continuation);
                EH(-1,"\t");
		break;		/* duh */
	      }
#ifdef PARALLEL
              check_parallel_error("Bad Continuation");
#endif

	    /* MMH: Needed to put this in, o/w it may find that the
	     * solution and residual HAPPEN to satisfy the convergence
	     * criterion for the next newton solve...
	     */
	    find_and_set_Dirichlet(x, xdot, exo, dpi);

            exchange_dof(cx, dpi, x);

	    /*    Should be doing first order prediction on ACs
	     *    but for now, just reset the AC variables
	     */
	    if( nAC > 0)
	      {
		dcopy1(nAC, x_AC_old, x_AC);
		for(iAC=0 ; iAC<nAC ; iAC++)
			{ update_parameterAC(iAC,x_AC, cx, exo, dpi);}
	      }
	  }   /*  end of !converged */
	  
      } while (converged == 0);

      /*
       * CONVERGED
       */
      nt++;

      if( Continuation == ALC_ZEROTH ) {
        DPRINTF(stderr, "\n\tStep accepted, parameter = %10.6e\n", path1);
       }
      else {
        DPRINTF(stderr, "\tStep accepted, parameter = %10.6e\n", path1);
       }

      /* 
       * check path step error, if too large do not enlarge path step 
       */
      if ((ni == 1) && (n != 0) && (!const_delta_s))
	{
	  delta_s_new = path_step_control(num_total_nodes, 
					  delta_s, delta_s_old, 
					  x, 
					  eps, 
					  &success_ds, 
					  cont->use_var_norm, inewton);
	  if (delta_s_new > Delta_s_max) 
	    delta_s_new = Delta_s_max;
	}
      else
	{
	  success_ds = 1;
	  delta_s_new = delta_s;
	}
	  
      /* 
       * determine whether to print out the data or not 
       */
      i_print = 0;
      if (nt == step_print)
	{
	  i_print = 1;
	  step_print += cont->print_freq;
	}
	  
      if (alqALC == -1) 
	i_print = 1;
	  
      if (i_print)
	{
	  error = write_ascii_soln(x, resid_vector, numProcUnknowns,
				   x_AC, nAC, path1, file);
	  if ( error )
	    DPRINTF(stdout, "%s:  error writing ASCII soln file\n", yo);
	  if (Write_Intermediate_Solutions == 0 ) {
	    error = write_solution(ExoFileOut, resid_vector, x, x_sens_p, 
				   x_old, xdot, tev, tev_post, NULL, rd, gindex,
				   p_gsize, gvec, gvec_elem, &nprint, 
				   delta_s, theta, path1, exo, dpi);
	      nprint++;
	  }
	}
      
      /*
       * backup old solutions
       * can use previous solutions for prediction one day
       */
      dcopy1(numProcUnknowns,x_older,x_oldest);
      dcopy1(numProcUnknowns,x_old,x_older);
      dcopy1(numProcUnknowns, x, x_old);
      dcopy1(numProcUnknowns, x_sens_temp, x_sens);

      delta_s_oldest = delta_s_older;
      delta_s_older = delta_s_old;
      delta_s_old = delta_s;
      delta_s = delta_s_new;
  
      if( nAC > 0)
	dcopy1(nAC, x_AC, x_AC_old);

      /*
       * INCREMENT/DECREMENT PARAMETER
       */
      path  = path1;
	  
      switch (aldALC)
	{
	case -1: 
	  path1 = path - delta_s;
	  break;
	case +1: 
	  path1 = path + delta_s;
	  break;
	default:
	  DPRINTF(stderr, "%s: Bad aldALC, %d\n", yo, aldALC);
          EH(-1,"\t");
	  break;		/* duh */
	}

#ifdef PARALLEL
      check_parallel_error("Bad aldALC");
#endif
      /*
       * ADJUST NATURAL PARAMETER
       */
      update_parameterC(path1, x, xdot, delta_s, 
			cx, exo, dpi);

      /*
	display_parameterC(path1, x, xdot, delta_s, 
	cx, exo, dpi);
      */		   

      /*
       * GET FIRST ORDER PREDICTION
       */
      switch (Continuation)
	{
	case ALC_ZEROTH:
	  break;
	case  ALC_FIRST:
	  switch (aldALC)
	    {
	    case -1: 
	      v1add(numProcUnknowns, &x[0], -delta_s, &x_sens[0]);
	      break;
	    case +1: 
	      v1add(numProcUnknowns, &x[0], +delta_s, &x_sens[0]);
	      break;
	    default:
	      DPRINTF(stderr, "%s: Bad aldALC, %d\n", yo, aldALC);
              EH(-1,"\t");
	      break;		/* duh */
	    }
	  break;
	}
#ifdef PARALLEL
      check_parallel_error("Bad aldALC");
#endif

      /*
       * CHECK END CONTINUATION
       */
      /*
      if (alqALC == -1)
	alqALC = 0;
      else
	alqALC = 1;
      */

      if (alqALC == -1)
	{
	  DPRINTF(stderr,"\n\n\t I will continue no more!\n\t No more continuation for you!\n");
	  goto free_and_clear;
	}
    } /* for(n = 0; n < MaxPathSteps; n++) */

  if(n == MaxPathSteps &&
     aldALC * (lambdaEnd - path) > 0)
    {
      DPRINTF(stderr, "\n\tFailed to reach end of hunt in maximum number of successful steps (%d).\n\tSorry.\n",
	      MaxPathSteps);
      EH(-1,"\t");
    }
#ifdef PARALLEL
      check_parallel_error("Continuation error");
#endif

  /*
   * DONE CONTINUATION
   */
 free_and_clear: 

  /*
   * Transform the node point coordinates according to the
   * displacements and write out all the results using the
   * displaced coordinates. Set the displacement field to
   * zero, too.
   */
  if (Anneal_Mesh)
    {
#ifdef DEBUG
      fprintf(stderr, "%s: anneal_mesh()...\n", yo);
#endif
      err = anneal_mesh(x, tev, tev_post, NULL, rd, path1, exo, dpi);
#ifdef DEBUG
      fprintf(stderr, "%s: anneal_mesh()-done\n", yo);
#endif
      EH(err, "anneal_mesh() bad return.");
    }
#ifdef PARALLEL
      check_parallel_error("Trouble annealing mesh");
#endif

  /* 
   * Free a bunch of variables that aren't needed anymore 
   */
  safer_free((void **) &ROT_Types);
  safer_free((void **) &node_to_fill);

  sl_free(matrix_systems_mask, ams);

  for (i = 0; i < NUM_ALSS; i++)
    safer_free((void **) &(ams[i]));

  safer_free( (void **) &resid_vector);
  safer_free( (void **) &resid_vector_sens);
  safer_free( (void **) &scale);
  safer_free( (void **) &x);

  if (nAC > 0)
    {
      safer_free( (void **) &x_AC);
      safer_free( (void **) &x_AC_old);
    }

  safer_free( (void **) &x_old); 
  safer_free( (void **) &x_older); 
  safer_free( (void **) &x_oldest); 
  safer_free( (void **) &xdot); 
  safer_free( (void **) &xdot_old); 
  safer_free( (void **) &x_update); 

  safer_free( (void **) &x_sens); 
  safer_free( (void **) &x_sens_temp); 

  if((nn_post_data_sens+nn_post_fluxes_sens) > 0)
          Dmatrix_death(x_sens_p,num_pvector,NumUnknowns[pg->imtrx]);

  for(i = 0; i < MAX_NUMBER_MATLS; i++) {
    for(n = 0; n < MAX_MODES; n++) {
      safer_free((void **) &(ve_glob[i][n]->gn));
      safer_free((void **) &(ve_glob[i][n]));
    }
    safer_free((void **) &(vn_glob[i]));
  }

  sl_free(matrix_systems_mask, ams);

  for (i = 0; i < NUM_ALSS; i++)
    safer_free((void **) &(ams[i]));

  safer_free( (void **) &gvec);

  i = 0;
  for ( eb_indx = 0; eb_indx < exo->num_elem_blocks; eb_indx++ )
    {
      for ( ev_indx = 0; ev_indx < rd->nev; ev_indx++ ) {
	if (exo->elem_var_tab[i++] == 1) {
	  safer_free((void **) &(gvec_elem [eb_indx][ev_indx]));
	}
      }
      safer_free((void **) &(gvec_elem [eb_indx]));
    }

  safer_free( (void **) &gvec_elem); 

  safer_free( (void **) &rd); 
  safer_free( (void **) &Local_Offset);
  safer_free( (void **) &Dolphin); 
  safer_free( (void **) &Num_Unknowns_Node); 
  safer_free( (void **) &First_Unknown); 

  fclose(file);

  return;

} /* END of routine continue_problem  */
/*******************************************************************************/
/*****************************************************************************/
/*  END of file rf_conti.c  */
/*****************************************************************************/