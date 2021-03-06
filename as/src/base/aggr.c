/*
 * aggr.c
 *
 * Copyright (C) 2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

#include "base/aggr.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "aerospike/as_aerospike.h"
#include "aerospike/as_list.h"
#include "aerospike/as_module.h"
#include "aerospike/as_rec.h"
#include "aerospike/as_result.h"
#include "aerospike/as_stream.h"
#include "aerospike/as_udf_context.h"
#include "aerospike/as_val.h"
#include "aerospike/mod_lua.h"
#include "citrusleaf/cf_atomic.h"
#include "citrusleaf/cf_ll.h"

#include "fault.h"

#include "base/datamodel.h"
#include "base/proto.h"
#include "base/thr_scan.h"
#include "base/transaction.h"
#include "base/udf_memtracker.h"
#include "base/udf_record.h"
#include "storage/storage.h"


extern const as_list_hooks udf_arglist_hooks;

static int
as_aggr_aerospike_log(const as_aerospike * a, const char * file, const int line, const int lvl, const char * msg)
{
	cf_fault_event(AS_AGGR, lvl, file, NULL, line, (char *) msg);
	return 0;
}

static const as_aerospike_hooks as_aggr_aerospike_hooks = {
	.open_subrec      = NULL,
	.close_subrec     = NULL,
	.update_subrec    = NULL,
	.create_subrec    = NULL,
	.rec_update       = NULL,
	.rec_remove       = NULL,
	.rec_exists       = NULL,
	.log              = as_aggr_aerospike_log,
	.get_current_time = NULL,
	.destroy          = NULL
};

/**
 * Get a value for a bin of with the given key.
 */
static as_val *
query_record_get(const as_rec * rec, const char * name)
{
	query_record  * qrecord = (query_record *) rec->data;
	as_rec        * urec    = qrecord->urec;
	return as_rec_get(urec, name);
}

static uint32_t
query_record_ttl(const as_rec * rec)
{
	query_record * qrecord = (query_record *) rec->data;
	as_rec       * urec    = qrecord->urec;
	return as_rec_ttl(urec);
}

static uint16_t
query_record_gen(const as_rec * rec)
{
	query_record * qrecord = (query_record *) rec->data;
	as_rec       * urec    = qrecord->urec;
	return as_rec_gen(urec);
}

static int
query_record_bin_names(const as_rec * rec, as_rec_bin_names_callback callback, void * context)
{
	query_record * qrecord = (query_record *) rec->data;
	as_rec       * urec    = qrecord->urec;
	return as_rec_bin_names(urec, callback, context);
}

static as_bytes *
query_record_digest(const as_rec * rec)
{
	query_record * qrecord = (query_record *) rec->data;
	as_rec       * urec    = qrecord->urec;
	return as_rec_digest(urec);
}

static as_val *
query_record_key(const as_rec * rec)
{
	query_record * qrecord = (query_record *) rec->data;
	as_rec       * urec    = qrecord->urec;
	return as_rec_key(urec);
}

static const char *
query_record_setname(const as_rec * rec)
{
	query_record * qrecord = (query_record *) rec->data;
	as_rec       * urec    = qrecord->urec;
	return as_rec_setname(urec);
}

// Not write operation allowed on the query_record
const as_rec_hooks query_record_hooks = {
	.get        = query_record_get,
	.set        = NULL,
	.remove     = NULL,
	.ttl        = query_record_ttl,
	.gen        = query_record_gen,
	.bin_names  = query_record_bin_names,
	.destroy    = NULL,
	.digest     = query_record_digest,
	.key        = query_record_key,
	.setname    = query_record_setname
};


extern const as_rec_hooks udf_record_hooks;
//extern const as_rec_hooks query_record_hooks;

#define AS_AGGR_ERR AS_QUERY_ERR
#define AS_AGGR_OK AS_QUERY_OK

int 
as_aggr__process(as_aggr_call * ap_call, cf_ll * ap_recl, void * udata, as_result * ap_res)
{
	// input stream
	int ret           = AS_QUERY_OK;
	as_index_ref    lr_ref;
	lr_ref.skip_lock   = false;
	as_storage_rd   l_rd;
	bzero(&l_rd, sizeof(as_storage_rd));
	as_transaction  l_tr;

	udf_record l_urecord = {
		.tr                 = &l_tr,
		.r_ref              = &lr_ref,
		.rd                 = &l_rd,
		.updates            = { {"", NULL} }, // statically initialized
		.nupdates           = 0,
		.particle_data      = NULL,
		.cur_particle_data  = NULL,
		.end_particle_data  = NULL,
		.flag               = UDF_RECORD_FLAG_ISVALID ,
		.starting_memory_bytes = 0,
	};
	l_urecord.flag |= UDF_RECORD_FLAG_ALLOW_DESTROY;

	as_rec                  l_urec;
	query_record l_qrecord = {
		.urec          = &l_urec,
		.caller        = ap_call->caller,
		.urecord       = &l_urecord,
		.read          = false,
	};
	as_rec_init(&l_urec, &l_urecord, &udf_record_hooks);
	as_rec                  l_qrec;
	as_rec_init(&l_qrec, &l_qrecord, &query_record_hooks);

	as_aggr_istream l_aggr_istream = {
		.dt          = NULL,
		.rec         = &l_qrec,
		.iter        = cf_ll_getIterator(ap_recl, true /*forward*/),
		.ns          = ap_call->ns,
		.get_type    = ap_call->caller_intf->get_type
	};

	if (!l_aggr_istream.iter) {
		cf_warning (AS_AGGR, "Could not set up iterator .. possibly out of memory .. Aborting Query !!");
		ap_call->caller_intf->set_error(ap_call->caller) ; //caller set error
		cf_debug(AS_AGGR, "AGGR caller %p Aborted at %s:%d", ap_call->caller, __FILE__, __LINE__);
		ret = AS_AGGR_ERR;//AS_QUERY_ERR;
		goto Cleanup;
	}

	as_aerospike l_as;
	as_aerospike_init(&l_as, NULL, &as_aggr_aerospike_hooks);

	// Input Stream
	as_stream l_istream;
	as_stream_init(&l_istream, &l_aggr_istream, (ap_call->istream_hooks));

	// Output stream
	as_stream l_ostream;
	if ( udata ) {
		as_stream_init(&l_ostream, udata, (ap_call->ostream_hooks));
	} else {
		as_stream_init(&l_ostream, ap_call->caller, (ap_call->ostream_hooks));
	}

	// Argument list
	as_list l_arglist;
	as_list_init(&l_arglist, ap_call->arglist, &udf_arglist_hooks);

	// Execute the stream operations
	mem_tracker l_aggr_mem_tracker = {
		.udata = &ap_call->caller,//&call->qtr, 
		.cb    = ap_call->caller_intf->mem_op, //mem_op
	};
	udf_memtracker_setup(&l_aggr_mem_tracker);

	as_udf_context l_ctx = {
		.as         = &l_as,
		.timer      = NULL,
		.memtracker = NULL
	};
	ret = as_module_apply_stream(&mod_lua, &l_ctx, ap_call->filename, ap_call->function, &l_istream, &l_arglist, &l_ostream, ap_res);

	cf_debug(AS_QUERY, " Apply Stream with %s %s %p %p %p ret=%d", ap_call->filename, ap_call->function, &l_istream, &l_arglist, &l_ostream, ret);
	udf_memtracker_cleanup();

	if (ret) {
		ap_call->caller_intf->set_error(ap_call->caller);
	}

	as_list_destroy(&l_arglist);


Cleanup:
	if (l_aggr_istream.iter) {
		cf_ll_releaseIterator(l_aggr_istream.iter);
		l_aggr_istream.iter = NULL;
	}
	if (l_qrecord.read) {
		udf_record_close(l_qrecord.urecord, true);
		l_qrecord.read       = false;
	}
	return ret;
}

/*
 * Function as_aggr_call_init
 *
 * Returns -
 * 		AS_QUERY_OK  - On success
 *		AS_QUERY_ERR - On failure
 *
 * Notes -
 *
 * TODO -
 * 		Error return values could be better.
 *
 */
int
as_aggr_call_init(as_aggr_call * call, as_transaction * txn, void *caller,const as_aggr_caller_intf * caller_intf, const as_stream_hooks * istream_hooks, const as_stream_hooks * ostream_hooks, as_namespace *ns, bool is_scan) //no need to worry about namespace as it's inside g_config 
{
	if ((!caller) || (!txn) || (!call)) return -1;

	call->active = false;
	// Check if type is aggregation
	as_msg_field *  op = NULL;
	op = as_msg_field_get(&txn->msgp->msg, AS_MSG_FIELD_TYPE_UDF_OP);
	if (!op) {
		cf_detail(AS_QUERY, "not a aggregation 1");
		return AS_AGGR_ERR;
	}
	byte optype;
	memcpy(&optype, (byte *)op->data, sizeof(optype));
	if(!is_scan) {
		if (optype != AS_QUERY_UDF_OP_AGGREGATE) {
			cf_detail(AS_QUERY, "not a aggregation 2");
			return AS_AGGR_ERR;
		}
	} else {
		if (optype != AS_SCAN_UDF_OP_AGGREGATE) {
			cf_detail(AS_QUERY, "not a aggregation 3");
			return AS_AGGR_ERR;
		}
	}


	as_msg_field *  filename = NULL;
	as_msg_field *  function = NULL;
	as_msg_field *  arglist =  NULL;

	filename = as_msg_field_get(&txn->msgp->msg, AS_MSG_FIELD_TYPE_UDF_FILENAME);
	if ( filename ) {
		function = as_msg_field_get(&txn->msgp->msg, AS_MSG_FIELD_TYPE_UDF_FUNCTION);
		if ( function ) {
			arglist = as_msg_field_get(&txn->msgp->msg, AS_MSG_FIELD_TYPE_UDF_ARGLIST);
			if ( arglist ) {
				call->caller  = caller;
				call->caller_intf = caller_intf;
				call->istream_hooks = istream_hooks;
				call->ostream_hooks = ostream_hooks;
				call->ns = ns;
				as_msg_field_get_strncpy(filename, &call->filename[0], sizeof(call->filename));
				as_msg_field_get_strncpy(function, &call->function[0], sizeof(call->function));
				call->arglist = arglist;
				call->active = true;
				return AS_AGGR_OK;
			}
		}
	}
	call->caller = NULL;
	call->filename[0] = 0;
	call->function[0] = 0;
	call->arglist = NULL;
	return AS_AGGR_ERR;
}

/**
 * Frees memory inside a udf call
 *
 * @returns nothing
 */
void
as_aggr_call_destroy(as_aggr_call * call)
{
	call->arglist = NULL;
	call->caller     = NULL;
}

//declaration 
extern bool as_query_aggr_match_record(query_record * qrecord);

// only operates on the record as_val in the stream points to
// and updates the references ... this function has to acquire
// partition reservation and also the object lock. So if the UDF
// does something stupid the object lock is gonna get held for
// a while ... there has to be timeout mechanism in here I think
as_val *
as_aggr_istream_read(const as_stream *s) 
{
	as_aggr_istream *aggr_istream = as_stream_source(s);

	if (!aggr_istream->iter) {
		return NULL;
	}

	query_record   * qrecord = (query_record *) aggr_istream->rec->data; //Sumit: taking out query_record from istream->rec->data
	dig_arr_t      * dt      = aggr_istream->dt;

	if (!dt) {
		cf_ll_element * ele       = cf_ll_getNext(aggr_istream->iter);
		if (!ele) aggr_istream->dt = NULL;
		else      dt               = ((ll_recl_element*)ele)->dig_arr;
		aggr_istream->dt          = dt;
		aggr_istream->dtoffset    = 0;
	}

	if (qrecord->read) {
		cf_detail(AS_QUERY, "Close Record (%p,%d)", aggr_istream->dt,
				aggr_istream->dtoffset - 1);
		// Bypassing doing the direct destroy because we need to
		// avoid reducing the ref count. This rec (query_record
		// implementation of as_rec) is ref counted when passed from
		// here to Lua. If Lua access it even after moving to next
		// element in the stream it does it at its own risk. Record
		// may have changed under the hood.
		udf_record_close(qrecord->urecord, true); //Sumit: udf record close closing qrecord urecord
		qrecord->read = false;
	}

	if (!dt) return NULL;

	// Iterate through stream to get next digest and
	// populate record with it
	while (!qrecord->read) {
		if (dt->num == aggr_istream->dtoffset) {
			if (dt) {
				// Not releasing here.. will be released
				// by the query_agg_apply_stream in the end
			}
			dt = NULL;
			while (!dt) {
				cf_ll_element * ele = cf_ll_getNext(aggr_istream->iter);
				if (!ele) {
					cf_detail(AS_QUERY, "No More Nodes for this Lua Call");
					return NULL;
				}
				dt                             = ((ll_recl_element*)ele)->dig_arr;
			}
			aggr_istream->dtoffset = 0;
			aggr_istream->dt       = dt;
			cf_detail(AS_QUERY, "Moving Next List Node");
		}
		// NB: The offset moves forward here
		as_transaction * tr    =  qrecord->urecord->tr;
		as_namespace   * ns    =  aggr_istream->ns;
		as_index_ref   * r_ref =  qrecord->urecord->r_ref;

		AS_PARTITION_RESERVATION_INIT(tr->rsv);
		tr->rsv.ns   = ns;
		tr->keyd     = dt->digs[aggr_istream->dtoffset];
		qrecord->urecord->keyd = tr->keyd;
		cf_detail(AS_QUERY, "Open Record (%p,%d %"PRIu64", %"PRIu64")", aggr_istream->dt, aggr_istream->dtoffset);

		if (0 != as_partition_reserve_qnode(ns, as_partition_getid(tr->keyd), &tr->rsv)) {
			aggr_istream->dtoffset++;
			continue;
		}
		cf_atomic_int_incr(&g_config.dup_tree_count);
		r_ref->skip_lock = false;
		if (0 == udf_record_open(qrecord->urecord)) { //Sumit record open
			qrecord->read = true;
		}
		if (!qrecord->read) {
			cf_debug(AS_QUERY, "Failed to read record");
			as_partition_release(&tr->rsv);
		} else {
			if (aggr_istream->get_type() == AS_AGGR_QUERY) {
				if (!as_query_aggr_match_record(qrecord)) {
					cf_debug(AS_QUERY, "Close Record with invalid selection (%p,%d)", aggr_istream->dt, aggr_istream->dtoffset);
					udf_record_close(qrecord->urecord, true);
					qrecord->read = false;
					cf_atomic64_incr(&g_config.query_false_positives);
				} else {
					cf_detail(AS_QUERY, "Successfully read record");
				}
			} 
		}
		aggr_istream->dtoffset++;
	}
	return (as_val *)aggr_istream->rec;
}

#if 0
as_val *
tscan_agg_istream_read(const as_stream *s) //Sumit : read function
{
	as_aggr_istream *aggr_istream = as_stream_source(s);

	if (!aggr_istream->iter) {
		return NULL;
	}

	query_record   * qrecord = (query_record *) aggr_istream->rec->data; //Sumit: taking out query_record from istream->rec->data
	dig_arr_t      * dt      = aggr_istream->dt;

	if (!dt) {
		cf_ll_element * ele       = cf_ll_getNext(aggr_istream->iter);
		if (!ele) aggr_istream->dt = NULL;
		else      dt               = ((ll_recl_element*)ele)->dig_arr;
		aggr_istream->dt          = dt;
		aggr_istream->dtoffset    = 0;
	}

	if (qrecord->read) {
		cf_detail(AS_SCAN, "Close Record (%p,%d)", aggr_istream->dt,
				aggr_istream->dtoffset - 1);
		// Bypassing doing the direct destroy because we need to
		// avoid reducing the ref count. This rec (query_record
		// implementation of as_rec) is ref counted when passed from
		// here to Lua. If Lua access it even after moving to next
		// element in the stream it does it at its own risk. Record
		// may have changed under the hood.
		udf_record_close(qrecord->urecord, true); //Sumit: udf record close closing qrecord urecord
		qrecord->read = false;
	}

	if (!dt) return NULL;

	// Iterate through stream to get next digest and
	// populate record with it
	while (!qrecord->read) {
		if (dt->num == aggr_istream->dtoffset) {
			if (dt) {
				// Not releasing here.. will be released
				// by the query_agg_apply_stream in the end
			}
			dt = NULL;
			while (!dt) {
				cf_ll_element * ele = cf_ll_getNext(aggr_istream->iter);
				if (!ele) {
					cf_detail(AS_SCAN, "No More Nodes for this Lua Call");
					return NULL;
				}
				dt                             = ((ll_recl_element*)ele)->dig_arr;
				((ll_recl_element*)ele)->dig_arr = NULL;
			}
			aggr_istream->dtoffset = 0;
			aggr_istream->dt       = dt;
			cf_detail(AS_SCAN, "Moving Next List Node");
		}
		// NB: The offset moves forward here
		as_transaction * tr    =  qrecord->urecord->tr;
		as_namespace   * ns    =  aggr_istream->ns;
		as_index_ref   * r_ref =  qrecord->urecord->r_ref;

		AS_PARTITION_RESERVATION_INIT(tr->rsv);
		tr->rsv.ns   = ns;
		tr->keyd     = dt->digs[aggr_istream->dtoffset];
		cf_detail(AS_SCAN, "Open Record (%p,%d %"PRIu64", %"PRIu64")", aggr_istream->dt, aggr_istream->dtoffset);

		if (0 != as_partition_reserve_qnode(ns, as_partition_getid(tr->keyd), &tr->rsv)) {
			aggr_istream->dtoffset++;
			continue;
		}
		cf_atomic_int_incr(&g_config.dup_tree_count);
		r_ref->skip_lock = false; //Sumit check to make code common
		if (0 == udf_record_open(qrecord->urecord)) { //Sumit record open
			qrecord->read = true;
		}
		if (!qrecord->read) {
			cf_debug(AS_SCAN, "Failed to read record");
			as_partition_release(&tr->rsv);
		}
		aggr_istream->dtoffset++;
	}
	return (as_val *)aggr_istream->rec;
}
#endif
