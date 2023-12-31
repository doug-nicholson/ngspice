/* 
 * This file is part of the OSDI component of NGSPICE.
 * Copyright© 2022 SemiMod GmbH.
 * 
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. 
 *
 * Author: Pascal Kuthe <pascal.kuthe@semimod.de>
 */

#include "ngspice/iferrmsg.h"
#include "ngspice/klu.h"
#include "ngspice/memory.h"
#include "ngspice/ngspice.h"
#include "ngspice/typedefs.h"

#include "osdi.h"
#include "osdidefs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Handles any errors raised by the setup_instance and setup_model functions
 */
static int handle_init_info(OsdiInitInfo info, const OsdiDescriptor *descr) {
  if (info.flags & (EVAL_RET_FLAG_FATAL | EVAL_RET_FLAG_FINISH)) {
    return (E_PANIC);
  }

  if (info.num_errors == 0) {
    return (OK);
  }

  for (uint32_t i = 0; i < info.num_errors; i++) {
    OsdiInitError *err = &info.errors[i];
    switch (err->code) {
    case INIT_ERR_OUT_OF_BOUNDS: {
      char *param = descr->param_opvar[err->payload.parameter_id].name[0];
      printf("Parameter %s is out of bounds!\n", param);
      break;
    }
    default:
      printf("Unknown OSDO init error code %d!\n", err->code);
    }
  }
  free(info.errors);
  errMsg = tprintf("%i errors occurred during initialization", info.num_errors);
  return (E_PRIVATE);
}

/*
 * The OSDI instance data contains the `node_mapping` array.
 * Here an index is stored for each node. This function initializes this array
 * with its indicies {0, 1, 2, 3, .., n}.
 * The node collapsing information generated by setup_instance is used to
 * replace these initial indicies with those that a node is collapsed into.
 * For example collapsing nodes i and j sets node_mapping[i] = j.
 *
 * Terminals can never be collapsed in ngspice because they are allocated by
 * ngspice instead of OSDI. Therefore any node collapsing that involves nodes
 * `i < connected_terminals` is ignored.
 *
 * @param const OsdiDescriptor *descr The OSDI descriptor
 * @param void *inst The instance data connected_terminals
 * @param uint32_t connected_terminals The number of terminals that are not
 * internal nodes.
 *
 * @returns The number of nodes required after collapsing.
 * */
static uint32_t collapse_nodes(const OsdiDescriptor *descr, void *inst,
                               uint32_t connected_terminals) {
  /* access data inside instance */
  uint32_t *node_mapping =
      (uint32_t *)(((char *)inst) + descr->node_mapping_offset);
  bool *collapsed = (bool *)(((char *)inst) + descr->collapsed_offset);

  /* without collapsing just return the total number of nodes */
  uint32_t num_nodes = descr->num_nodes;

  /*  populate nodes with themselves*/
  for (uint32_t i = 0; i < descr->num_nodes; i++) {
    node_mapping[i] = i;
  }

  for (uint32_t i = 0; i < descr->num_collapsible; i++) {
    /* check if the collapse hint (V(x,y) <+ 0) was executed */
    if (!collapsed[i]) {
      continue;
    }

    uint32_t from = descr->collapsible[i].node_1;
    uint32_t to = descr->collapsible[i].node_2;

    /* terminals created by the simulator cannot be collapsed
     */
    if (node_mapping[from] < connected_terminals &&
        (to == UINT32_MAX || node_mapping[to] < connected_terminals ||
         node_mapping[to] == UINT32_MAX)) {
      continue;
    }
    /* ensure that to is always the smaller node */
    if (to != UINT32_MAX && node_mapping[from] < node_mapping[to]) {
      uint32_t temp = from;
      from = to;
      to = temp;
    }

    from = node_mapping[from];
    if (to != UINT32_MAX) {
      to = node_mapping[to];
    }

    /* replace nodes mapped to from with to and reduce the number of nodes */
    for (uint32_t j = 0; j < descr->num_nodes; j++) {
      if (node_mapping[j] == from) {
        node_mapping[j] = to;
      } else if (node_mapping[j] > from && node_mapping[j] != UINT32_MAX) {
        node_mapping[j] -= 1;
      }
    }
    num_nodes -= 1;
  }
  return num_nodes;
}

/* replace node mapping local to the current instance (created by
 * collapse_nodes) with global node indicies allocated with CKTmkVolt */
static void write_node_mapping(const OsdiDescriptor *descr, void *inst,
                               uint32_t *nodes) {
  uint32_t *node_mapping =
      (uint32_t *)(((char *)inst) + descr->node_mapping_offset);
  for (uint32_t i = 0; i < descr->num_nodes; i++) {
    if (node_mapping[i] == UINT32_MAX) {
      /* gnd node */
      node_mapping[i] = 0;
    } else {
      node_mapping[i] = nodes[node_mapping[i]];
    }
  }
}

/* NGSPICE state vectors for an instance are always continous so we just write
 * state_start .. state_start + num_state to state_idx */
static void write_state_ids(const OsdiDescriptor *descr, void *inst,
                            uint32_t state_start) {
  uint32_t *state_idx = (uint32_t *)(((char *)inst) + descr->state_idx_off);
  for (uint32_t i = 0; i < descr->num_states; i++) {
    state_idx[i] = state_start + i;
  }
}

static int init_matrix(SMPmatrix *matrix, const OsdiDescriptor *descr,
                       void *inst) {
  uint32_t *node_mapping =
      (uint32_t *)(((char *)inst) + descr->node_mapping_offset);

  double **jacobian_ptr_resist =
      (double **)(((char *)inst) + descr->jacobian_ptr_resist_offset);

  for (uint32_t i = 0; i < descr->num_jacobian_entries; i++) {
    uint32_t equation = descr->jacobian_entries[i].nodes.node_1;
    uint32_t unkown = descr->jacobian_entries[i].nodes.node_2;
    equation = node_mapping[equation];
    unkown = node_mapping[unkown];
    double *ptr = SMPmakeElt(matrix, (int)equation, (int)unkown);

    if (ptr == NULL) {
      return (E_NOMEM);
    }
    jacobian_ptr_resist[i] = ptr;
    uint32_t react_off = descr->jacobian_entries[i].react_ptr_off;
    // complex number for ac analysis
    if (react_off != UINT32_MAX) {

      double **jacobian_ptr_react = (double **)(((char *)inst) + react_off);
      *jacobian_ptr_react = ptr + 1;
    }
  }
  return (OK);
}

int OSDIsetup(SMPmatrix *matrix, GENmodel *inModel, CKTcircuit *ckt,
              int *states) {
  OsdiInitInfo init_info;
  OsdiNgspiceHandle handle;
  GENmodel *gen_model;
  int res = (OK);
  int error;
  CKTnode *tmp;
  GENinstance *gen_inst;
  int err;

  OsdiRegistryEntry *entry = osdi_reg_entry_model(inModel);
  const OsdiDescriptor *descr = entry->descriptor;
  OsdiSimParas sim_params_ = get_simparams(ckt);
  OsdiSimParas *sim_params = &sim_params_;

  /* setup a temporary buffer */
  uint32_t *node_ids = TMALLOC(uint32_t, descr->num_nodes);

  /* determine the number of states required by each instance */
  int num_states = (int)descr->num_states;
  for (uint32_t i = 0; i < descr->num_nodes; i++) {
    if (descr->nodes[i].react_residual_off != UINT32_MAX) {
      num_states += 2;
    }
  }

  for (gen_model = inModel; gen_model; gen_model = gen_model->GENnextModel) {
    void *model = osdi_model_data(gen_model);

    /* setup model parameter (setup_model)*/
    handle = (OsdiNgspiceHandle){.kind = 1, .name = gen_model->GENmodName};
    descr->setup_model((void *)&handle, model, sim_params, &init_info);
    res = handle_init_info(init_info, descr);
    if (res) {
      errRtn = "OSDI setup_model";
      continue;
    }

    for (gen_inst = gen_model->GENinstances; gen_inst;
         gen_inst = gen_inst->GENnextInstance) {
      void *inst = osdi_instance_data(entry, gen_inst);

      /* special handling for temperature parameters */
      double temp = ckt->CKTtemp;
      OsdiExtraInstData *extra_inst_data =
          osdi_extra_instance_data(entry, gen_inst);
      if (extra_inst_data->temp_given) {
        temp = extra_inst_data->temp;
      }
      if (extra_inst_data->dt_given) {
        temp += extra_inst_data->dt;
      }

      /* find number of connected ports to allow evaluation of $port_connected
       * and to handle node collapsing correctly later
       * */
      int *terminals = (int *)(gen_inst + 1);
      uint32_t connected_terminals = descr->num_terminals;
      for (uint32_t i = 0; i < descr->num_terminals; i++) {
        if (terminals[i] == -1) {
          connected_terminals = i;
          break;
        }
      }

      /* calculate op independent data, init instance parameters and determine
       which collapsing occurs*/
      handle = (OsdiNgspiceHandle){.kind = 2, .name = gen_inst->GENname};
      descr->setup_instance((void *)&handle, inst, model, temp,
                            connected_terminals, sim_params, &init_info);
      res = handle_init_info(init_info, descr);
      if (res) {
        errRtn = "OSDI setup_instance";
        continue;
      }

      /* setup the instance nodes */

      uint32_t num_nodes = collapse_nodes(descr, inst, connected_terminals);
      /* copy terminals */
      memcpy(node_ids, gen_inst + 1, sizeof(int) * connected_terminals);
      /* create internal nodes as required */
      for (uint32_t i = connected_terminals; i < num_nodes; i++) {
        // TODO handle currents  correctly
        if (descr->nodes[i].is_flow) {
          error = CKTmkCur(ckt, &tmp, gen_inst->GENname, descr->nodes[i].name);
        } else {
          error = CKTmkVolt(ckt, &tmp, gen_inst->GENname, descr->nodes[i].name);
        }
        if (error)
          return (error);
        node_ids[i] = (uint32_t)tmp->number;
        // TODO nodeset?
      }
      write_node_mapping(descr, inst, node_ids);

      /* now that we have the node mapping we can create the matrix entries */
      err = init_matrix(matrix, descr, inst);
      if (err) {
        return err;
      }

      /* reserve space in the state vector*/
      gen_inst->GENstate = *states;
      write_state_ids(descr, inst, (uint32_t)*states);
      *states += num_states;
    }
  }

  free(node_ids);

  return res;
}

/* OSDI does not differentiate between setup and temperature update so we just
 * call the setup routines again and assume that  node collapsing (and therefore
 * node mapping) stays the same
 */
extern int OSDItemp(GENmodel *inModel, CKTcircuit *ckt) {
  OsdiInitInfo init_info;
  OsdiNgspiceHandle handle;
  GENmodel *gen_model;
  int res = (OK);
  GENinstance *gen_inst;

  OsdiRegistryEntry *entry = osdi_reg_entry_model(inModel);
  const OsdiDescriptor *descr = entry->descriptor;

  OsdiSimParas sim_params_ = get_simparams(ckt);
  OsdiSimParas *sim_params = &sim_params_;

  for (gen_model = inModel; gen_model != NULL;
       gen_model = gen_model->GENnextModel) {
    void *model = osdi_model_data(gen_model);

    handle = (OsdiNgspiceHandle){.kind = 4, .name = gen_model->GENmodName};
    descr->setup_model((void *)&handle, model, sim_params, &init_info);
    res = handle_init_info(init_info, descr);
    if (res) {
      errRtn = "OSDI setup_model (OSDItemp)";
      continue;
    }

    for (gen_inst = gen_model->GENinstances; gen_inst != NULL;
         gen_inst = gen_inst->GENnextInstance) {
      void *inst = osdi_instance_data(entry, gen_inst);

      // special handleing for temperature parameters
      double temp = ckt->CKTtemp;
      OsdiExtraInstData *extra_inst_data =
          osdi_extra_instance_data(entry, gen_inst);
      if (extra_inst_data->temp_given) {
        temp = extra_inst_data->temp;
      }
      if (extra_inst_data->dt_given) {
        temp += extra_inst_data->dt;
      }

      handle = (OsdiNgspiceHandle){.kind = 2, .name = gen_inst->GENname};
      /* find number of connected ports to allow evaluation of $port_connected
       * and to handle node collapsing correctly later
       * */
      int *terminals = (int *)(gen_inst + 1);
      uint32_t connected_terminals = descr->num_terminals;
      for (uint32_t i = 0; i < descr->num_terminals; i++) {
        if (terminals[i] == -1) {
          connected_terminals = i;
          break;
        }
      }

      descr->setup_instance((void *)&handle, inst, model, temp,
                            connected_terminals, sim_params, &init_info);
      res = handle_init_info(init_info, descr);
      if (res) {
        errRtn = "OSDI setup_instance (OSDItemp)";
        continue;
      }
      // TODO check that there are no changes in node collapse?
    }
  }
  return res;
}

/* delete internal nodes
 */
extern int OSDIunsetup(GENmodel *inModel, CKTcircuit *ckt) {
  GENmodel *gen_model;
  GENinstance *gen_inst;
  int num;

  OsdiRegistryEntry *entry = osdi_reg_entry_model(inModel);
  const OsdiDescriptor *descr = entry->descriptor;

  for (gen_model = inModel; gen_model != NULL;
       gen_model = gen_model->GENnextModel) {

    for (gen_inst = gen_model->GENinstances; gen_inst != NULL;
         gen_inst = gen_inst->GENnextInstance) {
      void *inst = osdi_instance_data(entry, gen_inst);

      // reset is collapsible
      bool *collapsed = (bool *)(((char *)inst) + descr->collapsed_offset);
      memset(collapsed, 0, sizeof(bool) * descr->num_collapsible);

      uint32_t *node_mapping =
          (uint32_t *)(((char *)inst) + descr->node_mapping_offset);
      for (uint32_t i = 0; i < descr->num_nodes; i++) {
        num = (int)node_mapping[i];
        // hand coded implementations just know which nodes were collapsed
        // however nodes may be collapsed multiple times so we can't easily use
        // an approach like that instead we delete all nodes
        // Deleting twiche with CKLdltNNum is fine (entry is already removed
        // from the linked list and therefore no action is taken).
        // However CKTdltNNum (rightfully) throws an error when trying to delete
        // an external node. Therefore we need to check for each node that it is
        // an internal node
        if (ckt->prev_CKTlastNode->number &&
            num > ckt->prev_CKTlastNode->number) {
          CKTdltNNum(ckt, num);
        }
      }
    }
  }
  return (OK);
}
#ifdef KLU
#include "ngspice/klu-binding.h"
static int init_matrix_klu(SMPmatrix *matrix, const OsdiDescriptor *descr,
                           void *inst, double **inst_matrix_ptrs) {
  BindElement tmp;
  BindElement *matched;
  BindElement *bindings = matrix->SMPkluMatrix->KLUmatrixBindStructCOO;
  size_t nz = (size_t)matrix->SMPkluMatrix->KLUmatrixLinkedListNZ;
  uint32_t *node_mapping =
      (uint32_t *)(((char *)inst) + descr->node_mapping_offset);
  double **jacobian_ptr_resist =
      (double **)(((char *)inst) + descr->jacobian_ptr_resist_offset);

  for (uint32_t i = 0; i < descr->num_jacobian_entries; i++) {
    uint32_t equation = descr->jacobian_entries[i].nodes.node_1;
    uint32_t unkown = descr->jacobian_entries[i].nodes.node_2;
    equation = node_mapping[equation];
    unkown = node_mapping[unkown];
    if (equation != 0 && unkown != 0) {
      tmp.COO = jacobian_ptr_resist[i];
      tmp.CSC = NULL;
      tmp.CSC_Complex = NULL;
      matched = (BindElement *)bsearch(&tmp, bindings, nz, sizeof(BindElement),
                                       BindCompare);
      if (matched == NULL) {
        printf("Ptr %p not found in BindStruct Table\n",
               jacobian_ptr_resist[i]);
        return (E_PANIC);
      }
      uint32_t react_off = descr->jacobian_entries[i].react_ptr_off;
      // complex number for ac analysis
      if (react_off != UINT32_MAX) {
        double **jacobian_ptr_react = (double **)(((char *)inst) + react_off);
        *jacobian_ptr_react = matched->CSC_Complex + 1;
      }
      jacobian_ptr_resist[i] = matched->CSC;
      inst_matrix_ptrs[2 * i] = matched->CSC;
      inst_matrix_ptrs[2 * i + 1] = matched->CSC_Complex;
    }
  }
  return (OK);
}

static int update_matrix_klu(const OsdiDescriptor *descr, void *inst,
                             double **inst_matrix_ptrs, bool complex) {
  uint32_t *node_mapping =
      (uint32_t *)(((char *)inst) + descr->node_mapping_offset);
  double **jacobian_ptr_resist =
      (double **)(((char *)inst) + descr->jacobian_ptr_resist_offset);

  for (uint32_t i = 0; i < descr->num_jacobian_entries; i++) {
    uint32_t equation = descr->jacobian_entries[i].nodes.node_1;
    uint32_t unkown = descr->jacobian_entries[i].nodes.node_2;
    equation = node_mapping[equation];
    unkown = node_mapping[unkown];
    if (equation != 0 && unkown != 0) {
      jacobian_ptr_resist[i] = inst_matrix_ptrs[2 * i + complex];
    }
  }
  return (OK);
}

int OSDIbindCSC(GENmodel *inModel, CKTcircuit *ckt) {

  OsdiRegistryEntry *entry = osdi_reg_entry_model(inModel);
  const OsdiDescriptor *descr = entry->descriptor;
  GENmodel *gen_model;
  GENinstance *gen_inst;

  NG_IGNORE(ckt);

  /* setup a temporary buffer 
  uint32_t *node_ids = TMALLOC(uint32_t, descr->num_nodes);*/

  for (gen_model = inModel; gen_model; gen_model = gen_model->GENnextModel) {
    /* void *model = osdi_model_data(gen_model); unused */
    for (gen_inst = gen_model->GENinstances; gen_inst;
         gen_inst = gen_inst->GENnextInstance) {
      void *inst = osdi_instance_data(entry, gen_inst);
      double **matrix_ptrs = osdi_instance_matrix_ptr(entry, gen_inst);
      int err = init_matrix_klu(ckt->CKTmatrix, descr, inst, matrix_ptrs);
      if (err != (OK)) {
        return err;
      }
    }
  }

  return (OK);
}
int OSDIupdateCSC(GENmodel *inModel, CKTcircuit *ckt, bool complex) {

  OsdiRegistryEntry *entry = osdi_reg_entry_model(inModel);
  const OsdiDescriptor *descr = entry->descriptor;
  GENmodel *gen_model;
  GENinstance *gen_inst;

  NG_IGNORE(ckt);

  /* setup a temporary buffer 
  uint32_t *node_ids = TMALLOC(uint32_t, descr->num_nodes);*/

  for (gen_model = inModel; gen_model; gen_model = gen_model->GENnextModel) {
    /* void *model = osdi_model_data(gen_model); unused */
    for (gen_inst = gen_model->GENinstances; gen_inst;
         gen_inst = gen_inst->GENnextInstance) {
      void *inst = osdi_instance_data(entry, gen_inst);
      double **matrix_ptrs = osdi_instance_matrix_ptr(entry, gen_inst);
      int err = update_matrix_klu(descr, inst, matrix_ptrs, complex);
      if (err != (OK)) {
        return err;
      }
    }
  }

  return (OK);
}
int OSDIbindCSCComplexToReal(GENmodel *inModel, CKTcircuit *ckt) {
  return OSDIupdateCSC(inModel, ckt, false);
}

int OSDIbindCSCComplex(GENmodel *inModel, CKTcircuit *ckt) {
  return OSDIupdateCSC(inModel, ckt, true);
}
#endif
