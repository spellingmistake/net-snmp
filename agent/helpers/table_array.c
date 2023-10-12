/*
 * table_array.c
 * $Id$
 *
 * Portions of this file are subject to the following copyright(s).  See
 * the Net-SNMP's COPYING file for more details and other copyrights
 * that may apply:
 *
 * Portions of this file are copyrighted by:
 * Copyright (c) 2016 VMware, Inc. All rights reserved.
 * Use is subject to license terms specified in the COPYING file
 * distributed with the Net-SNMP package.
 */

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-features.h>

#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <net-snmp/agent/table_array.h>

#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif

#include <net-snmp/agent/table.h>
#include <net-snmp/library/container.h>
#include <net-snmp/library/snmp_assert.h>

netsnmp_feature_child_of(table_array_all, mib_helpers);

netsnmp_feature_child_of(table_array_register,table_array_all);
netsnmp_feature_child_of(table_array_find_table_array_handler,table_array_all);
netsnmp_feature_child_of(table_array_extract_array_context,table_array_all);
netsnmp_feature_child_of(table_array_check_row_status,table_array_all);

#ifndef NETSNMP_FEATURE_REMOVE_TABLE_CONTAINER

/*
 * snmp.h:#define SNMP_MSG_INTERNAL_SET_BEGIN        -1 
 * snmp.h:#define SNMP_MSG_INTERNAL_SET_RESERVE1     0 
 * snmp.h:#define SNMP_MSG_INTERNAL_SET_RESERVE2     1 
 * snmp.h:#define SNMP_MSG_INTERNAL_SET_ACTION       2 
 * snmp.h:#define SNMP_MSG_INTERNAL_SET_COMMIT       3 
 * snmp.h:#define SNMP_MSG_INTERNAL_SET_FREE         4 
 * snmp.h:#define SNMP_MSG_INTERNAL_SET_UNDO         5 
 */

static const char *mode_name[] = {
    "Reserve 1",
    "Reserve 2",
    "Action",
    "Commit",
    "Free",
    "Undo"
};

/*
 * PRIVATE structure for holding important info for each table.
 */
typedef struct table_container_data_s {

   /** registration info for the table */
    netsnmp_table_registration_info *tblreg_info;

   /** container for the table rows */
   netsnmp_container          *table;

    /*
     * mutex_type                lock;
     */

   /** do we want to group rows with the same index
    * together when calling callbacks? */
    int             group_rows;

   /** callbacks for this table */
    netsnmp_table_array_callbacks *cb;

} table_container_data;

/** @defgroup table_array table_array
 *  Helps you implement a table when data can be stored locally. The data is stored in a sorted array, using a binary search for lookups.
 *  @ingroup table
 *
 *  The table_array handler is used (automatically) in conjunction
 *  with the @link table table@endlink handler. It is primarily
 *  intended to be used with the mib2c configuration file
 *  mib2c.array-user.conf.
 *
 *  The code generated by mib2c is useful when you have control of
 *  the data for each row. If you cannot control when rows are added
 *  and deleted (or at least be notified of changes to row data),
 *  then this handler is probably not for you.
 *
 *  This handler makes use of callbacks (function pointers) to
 *  handle various tasks. Code is generated for each callback,
 *  but will need to be reviewed and flushed out by the user.
 *
 *  NOTE NOTE NOTE: Once place where mib2c is somewhat lacking
 *  is with regards to tables with external indices. If your
 *  table makes use of one or more external indices, please
 *  review the generated code very carefully for comments
 *  regarding external indices.
 *
 *  NOTE NOTE NOTE: This helper, the API and callbacks are still
 *  being tested and may change.
 *
 *  The generated code will define a structure for storage of table
 *  related data. This structure must be used, as it contains the index
 *  OID for the row, which is used for keeping the array sorted. You can
 *  add addition fields or data to the structure for your own use.
 *
 *  The generated code will also have code to handle SNMP-SET processing.
 *  If your table does not support any SET operations, simply comment
 *  out the \#define \<PREFIX\>_SET_HANDLING (where \<PREFIX\> is your
 *  table name) in the header file.
 *
 *  SET processing modifies the row in-place. The duplicate_row
 *  callback will be called to save a copy of the original row.
 *  In the event of a failure before the commit phase, the
 *  row_copy callback will be called to restore the original row
 *  from the copy.
 *
 *  Code will be generated to handle row creation. This code may be
 *  disabled by commenting out the \#define \<PREFIX\>_ROW_CREATION
 *  in the header file.
 *
 *  If your table contains a RowStatus object, by default the
 *  code will not allow object in an active row to be modified.
 *  To allow active rows to be modified, remove the comment block
 *  around the \#define \<PREFIX\>_CAN_MODIFY_ACTIVE_ROW in the header
 *  file.
 *
 *  Code will be generated to maintain a secondary index for all
 *  rows, stored in a binary tree. This is very useful for finding
 *  rows by a key other than the OID index. By default, the functions
 *  for maintaining this tree will be based on a character string.
 *  NOTE: this will likely be made into a more generic mechanism,
 *  using new callback methods, in the near future.
 *
 *  The generated code contains many TODO comments. Make sure you
 *  check each one to see if it applies to your code. Examples include
 *  checking indices for syntax (ranges, etc), initializing default
 *  values in newly created rows, checking for row activation and
 *  deactivation requirements, etc.
 *
 * @{
 */

/**********************************************************************
 **********************************************************************
 *                                                                    *
 *                                                                    *
 * PUBLIC Registration functions                                      *
 *                                                                    *
 *                                                                    *
 **********************************************************************
 **********************************************************************/
/** register specified callbacks for the specified table/oid. If the
    group_rows parameter is set, the row related callbacks will be
    called once for each unique row index. Otherwise, each callback
    will be called only once, for all objects.
*/
int
netsnmp_table_container_register(netsnmp_handler_registration *reginfo,
                             netsnmp_table_registration_info *tabreg,
                             netsnmp_table_array_callbacks *cb,
                             netsnmp_container *container,
                             int group_rows)
{
    table_container_data *tad = SNMP_MALLOC_TYPEDEF(table_container_data);
    if (!tad)
        return SNMPERR_GENERR;
    tad->tblreg_info = tabreg;  /* we need it too, but it really is not ours */

    if (!cb) {
        snmp_log(LOG_ERR, "table_array registration with no callbacks\n" );
        free(tad); /* SNMP_FREE is overkill for local var */
        return SNMPERR_GENERR;
    }
    /*
     * check for required callbacks
     */
    if ((cb->can_set &&
         ((NULL==cb->duplicate_row) || (NULL==cb->delete_row) ||
          (NULL==cb->row_copy)) )) {
        snmp_log(LOG_ERR, "table_array registration with incomplete "
                 "callback structure.\n");
        free(tad); /* SNMP_FREE is overkill for local var */
        return SNMPERR_GENERR;
    }

    if (NULL==container) {
        tad->table = netsnmp_container_find("table_array");
        snmp_log(LOG_ERR, "table_array couldn't allocate container\n" );
        free(tad); /* SNMP_FREE is overkill for local var */
        return SNMPERR_GENERR;
    } else
        tad->table = container;
    if (NULL==tad->table->compare)
        tad->table->compare = netsnmp_compare_netsnmp_index;
    if (NULL==tad->table->ncompare)
        tad->table->ncompare = netsnmp_ncompare_netsnmp_index;
    
    tad->cb = cb;

    reginfo->handler->myvoid = tad;

    return netsnmp_register_table(reginfo, tabreg);
}

#ifndef NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_REGISTER
int
netsnmp_table_array_register(netsnmp_handler_registration *reginfo,
                             netsnmp_table_registration_info *tabreg,
                             netsnmp_table_array_callbacks *cb,
                             netsnmp_container *container,
                             int group_rows)
{
    netsnmp_mib_handler *handler =
        netsnmp_create_handler(reginfo->handlerName,
                               netsnmp_table_array_helper_handler);
    if (!handler ||
        (netsnmp_inject_handler(reginfo, handler) != SNMPERR_SUCCESS)) {
        snmp_log(LOG_ERR, "could not create table array handler\n");
        netsnmp_handler_free(handler);
        netsnmp_handler_registration_free(reginfo);
        return SNMP_ERR_GENERR;
    }

    return netsnmp_table_container_register(reginfo, tabreg, cb,
                                            container, group_rows);
}
#endif /* NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_REGISTER */

/** find the handler for the table_array helper. */
#ifndef NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_FIND_TABLE_ARRAY_HANDLER
netsnmp_mib_handler *
netsnmp_find_table_array_handler(netsnmp_handler_registration *reginfo)
{
    netsnmp_mib_handler *mh;
    if (!reginfo)
        return NULL;
    mh = reginfo->handler;
    while (mh) {
        if (mh->access_method == netsnmp_table_array_helper_handler)
            break;
        mh = mh->next;
    }

    return mh;
}
#endif /* NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_FIND_TABLE_ARRAY_HANDLER */

/** find the context data used by the table_array helper */
#ifndef NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_EXTRACT_ARRAY_CONTEXT
netsnmp_container      *
netsnmp_extract_array_context(netsnmp_request_info *request)
{
    return (netsnmp_container*)netsnmp_request_get_list_data(request, TABLE_ARRAY_NAME);
}
#endif /* NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_EXTRACT_ARRAY_CONTEXT */

/** this function is called to validate RowStatus transitions. */
#ifndef NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_CHECK_ROW_STATUS
int
netsnmp_table_array_check_row_status(netsnmp_table_array_callbacks *cb,
                                     netsnmp_request_group *ag,
                                     long *rs_new, long *rs_old)
{
    netsnmp_index *row_ctx;
    netsnmp_index *undo_ctx;
    if (!ag || !cb)
        return SNMPERR_GENERR;
    row_ctx  = ag->existing_row;
    undo_ctx = ag->undo_info;
    
    /*
     * xxx-rks: revisit row delete scenario
     */
    if (row_ctx) {
        /*
         * either a new row, or change to old row
         */
        /*
         * is it set to active?
         */
        if (RS_IS_GOING_ACTIVE(*rs_new)) {
            /*
             * is it ready to be active?
             */
            if ((NULL==cb->can_activate) ||
                cb->can_activate(undo_ctx, row_ctx, ag))
                *rs_new = RS_ACTIVE;
            else
                return SNMP_ERR_INCONSISTENTVALUE;
        } else {
            /*
             * not going active
             */
            if (undo_ctx) {
                /*
                 * change
                 */
                if (RS_IS_ACTIVE(*rs_old)) {
                    /*
                     * check pre-reqs for deactivation
                     */
                    if (cb->can_deactivate &&
                        !cb->can_deactivate(undo_ctx, row_ctx, ag)) {
                        return SNMP_ERR_INCONSISTENTVALUE;
                    }
                }
            } else {
                /*
                 * new row
                 */
            }

            if (*rs_new != RS_DESTROY) {
                if ((NULL==cb->can_activate) ||
                    cb->can_activate(undo_ctx, row_ctx, ag))
                    *rs_new = RS_NOTINSERVICE;
                else
                    *rs_new = RS_NOTREADY;
            } else {
                if (cb->can_delete && !cb->can_delete(undo_ctx, row_ctx, ag)) {
                    return SNMP_ERR_INCONSISTENTVALUE;
                }
                ag->row_deleted = 1;
            }
        }
    } else {
        /*
         * check pre-reqs for delete row
         */
        if (cb->can_delete && !cb->can_delete(undo_ctx, row_ctx, ag)) {
            return SNMP_ERR_INCONSISTENTVALUE;
        }
    }

    return SNMP_ERR_NOERROR;
}
#endif /* NETSNMP_FEATURE_REMOVE_TABLE_ARRAY_CHECK_ROW_STATUS */

/** @} */

/** @cond */
/**********************************************************************
 **********************************************************************
 **********************************************************************
 **********************************************************************
 *                                                                    *
 *                                                                    *
 *                                                                    *
 *                                                                    *
 * EVERYTHING BELOW THIS IS PRIVATE IMPLEMENTATION DETAILS.           *
 *                                                                    *
 *                                                                    *
 *                                                                    *
 *                                                                    *
 **********************************************************************
 **********************************************************************
 **********************************************************************
 **********************************************************************/

/**********************************************************************
 **********************************************************************
 *                                                                    *
 *                                                                    *
 * Structures, Utility/convenience functions                          *
 *                                                                    *
 *                                                                    *
 **********************************************************************
 **********************************************************************/
/*
 * context info for SET requests
 */
#ifndef NETSNMP_NO_WRITE_SUPPORT
typedef struct set_context_s {
    netsnmp_agent_request_info *agtreq_info;
    table_container_data *tad;
    int             status;
} set_context;
#endif /* NETSNMP_NO_WRITE_SUPPORT */

void
build_new_oid(netsnmp_handler_registration *reginfo,
              netsnmp_table_request_info *tblreq_info,
              netsnmp_index *row, netsnmp_request_info *current)
{
    oid             coloid[MAX_OID_LEN];

    if (!tblreq_info || !reginfo || !row || !current)
        return;

    memcpy(coloid, reginfo->rootoid, reginfo->rootoid_len * sizeof(oid));

    /** table.entry */
    coloid[reginfo->rootoid_len] = 1;

    /** table.entry.column */
    coloid[reginfo->rootoid_len + 1] = tblreq_info->colnum;

    /** table.entry.column.index */
    memcpy(&coloid[reginfo->rootoid_len + 2], row->oids,
           row->len * sizeof(oid));

    snmp_set_var_objid(current->requestvb, coloid,
                       reginfo->rootoid_len + 2 + row->len);
}

/**********************************************************************
 **********************************************************************
 *                                                                    *
 *                                                                    *
 * GET procession functions                                           *
 *                                                                    *
 *                                                                    *
 **********************************************************************
 **********************************************************************/
int
process_get_requests(netsnmp_handler_registration *reginfo,
                     netsnmp_agent_request_info *agtreq_info,
                     netsnmp_request_info *requests,
                     table_container_data * tad)
{
    int             rc = SNMP_ERR_NOERROR;
    netsnmp_request_info *current;
    netsnmp_index *row = NULL;
    netsnmp_table_request_info *tblreq_info;
    netsnmp_variable_list *var;

    /*
     * Loop through each of the requests, and
     * try to find the appropriate row from the container.
     */
    for (current = requests; current; current = current->next) {

        var = current->requestvb;
        DEBUGMSGTL(("table_array:get",
                    "  process_get_request oid:"));
        DEBUGMSGOID(("table_array:get", var->name,
                     var->name_length));
        DEBUGMSG(("table_array:get", "\n"));

        /*
         * skip anything that doesn't need processing.
         */
        if (current->processed != 0) {
            DEBUGMSGTL(("table_array:get", "already processed\n"));
            continue;
        }

        /*
         * Get pointer to the table information for this request. This
         * information was saved by table_helper_handler. When
         * debugging, we double check a few assumptions. For example,
         * the table_helper_handler should enforce column boundaries.
         */
        tblreq_info = netsnmp_extract_table_info(current);
        netsnmp_assert(tblreq_info->colnum <= tad->tblreg_info->max_column);

        if ((agtreq_info->mode == MODE_GETNEXT) ||
            (agtreq_info->mode == MODE_GETBULK)) {
            /*
             * find the row
             */
            row = netsnmp_table_index_find_next_row(tad->table, tblreq_info);
            if (!row) {
                /*
                 * no results found.
                 *
                 * xxx-rks: how do we skip this entry for the next handler,
                 * but still allow it a chance to hit another handler?
                 */
                DEBUGMSGTL(("table_array:get", "no row found\n"));
                netsnmp_set_request_error(agtreq_info, current,
                                          SNMP_ENDOFMIBVIEW);
                continue;
            }

            /*
             * * if data was found, make sure it has the column we want
             */
/* xxx-rks: add suport for sparse tables */

            /*
             * build new oid
             */
            build_new_oid(reginfo, tblreq_info, row, current);

        } /** GETNEXT/GETBULK */
        else {
            netsnmp_index index;
            index.oids = tblreq_info->index_oid;
            index.len = tblreq_info->index_oid_len;

            row = (netsnmp_index*)CONTAINER_FIND(tad->table, &index);
            if (!row) {
                DEBUGMSGTL(("table_array:get", "no row found\n"));
                netsnmp_set_request_error(agtreq_info, current,
                                          SNMP_NOSUCHINSTANCE);
                continue;
            }
        } /** GET */

        /*
         * get the data
         */
        rc = tad->cb->get_value(current, row, tblreq_info);

    } /** for ( ... requests ... ) */

    return rc;
}

/**********************************************************************
 **********************************************************************
 *                                                                    *
 *                                                                    *
 * SET procession functions                                           *
 *                                                                    *
 *                                                                    *
 **********************************************************************
 **********************************************************************/

void
group_requests(netsnmp_agent_request_info *agtreq_info,
               netsnmp_request_info *requests,
               netsnmp_container *request_group, table_container_data * tad)
{
    netsnmp_table_request_info *tblreq_info;
    netsnmp_index *row, *tmp, index;
    netsnmp_request_info *current;
    netsnmp_request_group *g;
    netsnmp_request_group_item *i;

    for (current = requests; current; current = current->next) {
        /*
         * skip anything that doesn't need processing.
         */
        if (current->processed != 0) {
            DEBUGMSGTL(("table_array:group",
                        "already processed\n"));
            continue;
        }

        /*
         * 3.2.1 Setup and paranoia
         * *
         * * Get pointer to the table information for this request. This
         * * information was saved by table_helper_handler. When
         * * debugging, we double check a few assumptions. For example,
         * * the table_helper_handler should enforce column boundaries.
         */
        row = NULL;
        tblreq_info = netsnmp_extract_table_info(current);
        netsnmp_assert(tblreq_info->colnum <= tad->tblreg_info->max_column);

        /*
         * search for index
         */
        index.oids = tblreq_info->index_oid;
        index.len = tblreq_info->index_oid_len;
        tmp = (netsnmp_index*)CONTAINER_FIND(request_group, &index);
        if (tmp) {
            DEBUGMSGTL(("table_array:group",
                        "    existing group:"));
            DEBUGMSGOID(("table_array:group", index.oids,
                         index.len));
            DEBUGMSG(("table_array:group", "\n"));
            g = (netsnmp_request_group *) tmp;
            i = SNMP_MALLOC_TYPEDEF(netsnmp_request_group_item);
            if (i == NULL)
                return;
            i->ri = current;
            i->tri = tblreq_info;
            i->next = g->list;
            g->list = i;

            /** xxx-rks: store map of colnum to request */
            continue;
        }

        DEBUGMSGTL(("table_array:group", "    new group"));
        DEBUGMSGOID(("table_array:group", index.oids,
                     index.len));
        DEBUGMSG(("table_array:group", "\n"));
        g = SNMP_MALLOC_TYPEDEF(netsnmp_request_group);
        i = SNMP_MALLOC_TYPEDEF(netsnmp_request_group_item);
        if (i == NULL || g == NULL) {
            SNMP_FREE(i);
            SNMP_FREE(g);
            return;
        }
        g->list = i;
        g->table = tad->table;
        i->ri = current;
        i->tri = tblreq_info;
        /** xxx-rks: store map of colnum to request */

        /*
         * search for row. all changes are made to the original row,
         * later, we'll make a copy in undo_info before we start processing.
         */
        row = g->existing_row = (netsnmp_index*)CONTAINER_FIND(tad->table, &index);
        if (!g->existing_row) {
            if (!tad->cb->create_row) {
#ifndef NETSNMP_NO_WRITE_SUPPORT
                if(MODE_IS_SET(agtreq_info->mode))
                    netsnmp_set_request_error(agtreq_info, current,
                                              SNMP_ERR_NOTWRITABLE);
                else
#endif /* NETSNMP_NO_WRITE_SUPPORT */
                    netsnmp_set_request_error(agtreq_info, current,
                                              SNMP_NOSUCHINSTANCE);
                free(g);
                free(i);
                continue;
            }
            /** use undo_info temporarily */
            row = g->existing_row = tad->cb->create_row(&index);
            if (!row) {
                /* xxx-rks : parameter to create_row to allow
                 * for better error reporting. */
                netsnmp_set_request_error(agtreq_info, current,
                                          SNMP_ERR_GENERR);
                free(g);
                free(i);
                continue;
            }
            g->row_created = 1;
        }

        g->index.oids = row->oids;
        g->index.len = row->len;

        CONTAINER_INSERT(request_group, g);

    } /** for( current ... ) */
}

#ifndef NETSNMP_NO_WRITE_SUPPORT
static void
release_netsnmp_request_group(void *g, void *v)
{
    netsnmp_request_group_item *tmp;
    netsnmp_request_group *group = g;

    if (!g)
        return;
    while (group->list) {
        tmp = group->list;
        group->list = tmp->next;
        free(tmp);
    }

    free(group);
}

static void
release_netsnmp_request_groups(void *vp)
{
    netsnmp_container *c = vp;
    CONTAINER_FOR_EACH(c, release_netsnmp_request_group, NULL);
    CONTAINER_FREE(c);
}

static void
process_set_group(void *o, void *c)
{
    /* xxx-rks: should we continue processing after an error?? */
    set_context           *context = (set_context *) c;
    netsnmp_request_group *ag = o;
    int                    rc = SNMP_ERR_NOERROR;

    switch (context->agtreq_info->mode) {

    case MODE_SET_RESERVE1:/** -> SET_RESERVE2 || SET_FREE */

        /*
         * if not a new row, save undo info
         */
        if (ag->row_created == 0) {
            if (context->tad->cb->duplicate_row)
                ag->undo_info = context->tad->cb->duplicate_row(ag->existing_row);
            else
                ag->undo_info = NULL;
            if (NULL == ag->undo_info) {
                rc = SNMP_ERR_RESOURCEUNAVAILABLE;
                break;
            }
        }
        
        if (context->tad->cb->set_reserve1)
            context->tad->cb->set_reserve1(ag);
        break;

    case MODE_SET_RESERVE2:/** -> SET_ACTION || SET_FREE */
        if (context->tad->cb->set_reserve2)
            context->tad->cb->set_reserve2(ag);
        break;

    case MODE_SET_ACTION:/** -> SET_COMMIT || SET_UNDO */
        if (context->tad->cb->set_action)
            context->tad->cb->set_action(ag);
        break;

    case MODE_SET_COMMIT:/** FINAL CHANCE ON SUCCESS */
        if (ag->row_created == 0) {
            /*
             * this is an existing row, has it been deleted?
             */
            if (ag->row_deleted == 1) {
                DEBUGMSGT((TABLE_ARRAY_NAME, "action: deleting row\n"));
                if (CONTAINER_REMOVE(ag->table, ag->existing_row) != 0) {
                    rc = SNMP_ERR_COMMITFAILED;
                    break;
                }
            }
        } else if (ag->row_deleted == 0) {
            /*
             * new row (that hasn't been deleted) should be inserted
             */
            DEBUGMSGT((TABLE_ARRAY_NAME, "action: inserting row\n"));
            if (CONTAINER_INSERT(ag->table, ag->existing_row) != 0) {
                rc = SNMP_ERR_COMMITFAILED;
                break;
            }
        }

        if (context->tad->cb->set_commit)
            context->tad->cb->set_commit(ag);

        /** no more use for undo_info, so free it */
        if (ag->undo_info) {
            context->tad->cb->delete_row(ag->undo_info);
            ag->undo_info = NULL;
        }

#if 0
        /* XXX-rks: finish row cooperative notifications
         * if the table has requested it, send cooperative notifications
         * for row operations.
         */
        if (context->tad->notifications) {
            if (ag->undo_info) {
                if (!ag->existing_row)
                    netsnmp_monitor_notify(EVENT_ROW_DEL);
                else
                    netsnmp_monitor_notify(EVENT_ROW_MOD);
            }
            else
                netsnmp_monitor_notify(EVENT_ROW_ADD);
        }
#endif

        if ((ag->row_created == 0) && (ag->row_deleted == 1)) {
            context->tad->cb->delete_row(ag->existing_row);
            ag->existing_row = NULL;
        }
        break;

    case MODE_SET_FREE:/** FINAL CHANCE ON FAILURE */
        if (context->tad->cb->set_free)
            context->tad->cb->set_free(ag);

        /** no more use for undo_info, so free it */
        if (ag->row_created == 1) {
            if (context->tad->cb->delete_row)
                context->tad->cb->delete_row(ag->existing_row);
            ag->existing_row = NULL;
        }
        else {
            if (context->tad->cb->delete_row)
                context->tad->cb->delete_row(ag->undo_info);
            ag->undo_info = NULL;
        }
        break;

    case MODE_SET_UNDO:/** FINAL CHANCE ON FAILURE */
        /*
         * status already set - don't change it now
         */
        if (context->tad->cb->set_undo)
            context->tad->cb->set_undo(ag);

        /*
         * no more use for undo_info, so free it
         */
        if (ag->row_created == 0) {
            /*
             * restore old values
             */
            context->tad->cb->row_copy(ag->existing_row, ag->undo_info);
            context->tad->cb->delete_row(ag->undo_info);
            ag->undo_info = NULL;
        }
        else {
            context->tad->cb->delete_row(ag->existing_row);
            ag->existing_row = NULL;
        }
        break;

    default:
        snmp_log(LOG_ERR, "unknown mode processing SET for "
                 "netsnmp_table_array_helper_handler\n");
        rc = SNMP_ERR_GENERR;
        break;
    }
    
    if (rc)
        netsnmp_set_request_error(context->agtreq_info,
                                  ag->list->ri, rc);
                                               
}

int
process_set_requests(netsnmp_agent_request_info *agtreq_info,
                     netsnmp_request_info *requests,
                     table_container_data * tad, char *handler_name)
{
    set_context         context;
    netsnmp_container  *request_group;

    /*
     * create and save structure for set info
     */
    request_group = (netsnmp_container*) netsnmp_agent_get_list_data
        (agtreq_info, handler_name);
    if (request_group == NULL) {
        netsnmp_data_list *tmp;
        request_group = netsnmp_container_find("request_group:"
                                               "table_container");
        request_group->compare = netsnmp_compare_netsnmp_index;
        request_group->ncompare = netsnmp_ncompare_netsnmp_index;

        DEBUGMSGTL(("table_array", "Grouping requests by oid\n"));

        tmp = netsnmp_create_data_list(handler_name,
                                       request_group,
                                       release_netsnmp_request_groups);
        netsnmp_agent_add_list_data(agtreq_info, tmp);
        /*
         * group requests.
         */
        group_requests(agtreq_info, requests, request_group, tad);
    }

    /*
     * process each group one at a time
     */
    context.agtreq_info = agtreq_info;
    context.tad = tad;
    context.status = SNMP_ERR_NOERROR;
    CONTAINER_FOR_EACH(request_group, process_set_group, &context);

    return context.status;
}
#endif /* NETSNMP_NO_WRITE_SUPPORT */


/**********************************************************************
 **********************************************************************
 *                                                                    *
 *                                                                    *
 * netsnmp_table_array_helper_handler()                               *
 *                                                                    *
 *                                                                    *
 **********************************************************************
 **********************************************************************/
int
netsnmp_table_array_helper_handler(netsnmp_mib_handler *handler,
                                   netsnmp_handler_registration *reginfo,
                                   netsnmp_agent_request_info *agtreq_info,
                                   netsnmp_request_info *requests)
{

    /*
     * First off, get our pointer from the handler. This
     * lets us get to the table registration information we
     * saved in get_table_array_handler(), as well as the
     * container where the actual table data is stored.
     */
    int             rc = SNMP_ERR_NOERROR;
    table_container_data *tad = (table_container_data *)handler->myvoid;

    if (agtreq_info->mode < 0 || agtreq_info->mode > 5) {
        DEBUGMSGTL(("table_array", "Mode %d, Got request:\n",
                    agtreq_info->mode));
    } else {
        DEBUGMSGTL(("table_array", "Mode %s, Got request:\n",
                    mode_name[agtreq_info->mode]));
    }

#ifndef NETSNMP_NO_WRITE_SUPPORT
    if (MODE_IS_SET(agtreq_info->mode)) {
        /*
         * netsnmp_mutex_lock(&tad->lock);
         */
        rc = process_set_requests(agtreq_info, requests,
                                  tad, handler->handler_name);
        /*
         * netsnmp_mutex_unlock(&tad->lock);
         */
    } else
#endif /* NETSNMP_NO_WRITE_SUPPORT */
        rc = process_get_requests(reginfo, agtreq_info, requests, tad);

    if (rc != SNMP_ERR_NOERROR) {
        DEBUGMSGTL(("table_array", "processing returned rc %d\n", rc));
    }
    
    /*
     * Now we've done our processing. If there is another handler below us,
     * call them.
     */
    if (handler->next) {
        rc = netsnmp_call_next_handler(handler, reginfo, agtreq_info, requests);
        if (rc != SNMP_ERR_NOERROR) {
            DEBUGMSGTL(("table_array", "next handler returned rc %d\n", rc));
        }
    }
    
    return rc;
}
#endif /* NETSNMP_FEATURE_REMOVE_TABLE_CONTAINER */
/** @endcond */
