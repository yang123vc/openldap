/* OpenLDAP WiredTiger backend */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2002-2015 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */
/* ACKNOWLEDGEMENTS:
 * This work was developed by HAMANO Tsukasa <hamano@osstech.co.jp>
 * based on back-bdb for inclusion in OpenLDAP Software.
 * WiredTiger is a product of MongoDB Inc.
 */

#include "back-wt.h"
#include "config.h"

static int wt_id2entry_put(
	Operation *op,
	WT_SESSION *session,
	Entry *e,
	const char *config )
{
	struct berval bv;
	WT_CURSOR *cursor = NULL;
	WT_ITEM item;
	int rc;

	rc = entry_encode( e, &bv );
	if(rc != LDAP_SUCCESS){
		return -1;
	}
	item.size = bv.bv_len;
	item.data = bv.bv_val;

	rc = session->open_cursor(session, WT_TABLE_ID2ENTRY, NULL,
							  config, &cursor);
	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			   LDAP_XSTRING(wt_id2entry_put)
			   ": open_cursor failed: %s (%d)\n",
			   wiredtiger_strerror(rc), rc, 0 );
		goto done;
	}
	cursor->set_key(cursor, e->e_id);
	cursor->set_value(cursor, e->e_ndn, &item);
	rc = cursor->insert(cursor);
	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			   LDAP_XSTRING(wt_id2entry_put)
			   ": insert failed: %s (%d)\n",
			   wiredtiger_strerror(rc), rc, 0 );
		goto done;
	}

done:
	ch_free( bv.bv_val );
	if(cursor){
		cursor->close(cursor);
	}
	return rc;
}

int wt_id2entry_add(
	Operation *op,
	WT_SESSION *session,
	Entry *e )
{
	return wt_id2entry_put(op, session, e, "overwrite=false");
}

int wt_id2entry_update(
	Operation *op,
	WT_SESSION *session,
	Entry *e )
{
	return wt_id2entry_put(op, session, e, "overwrite=true");
}

int wt_id2entry_delete(
	Operation *op,
	WT_SESSION *session,
	Entry *e )
{
	int rc;
	WT_CURSOR *cursor = NULL;
	rc = session->open_cursor(session, WT_TABLE_ID2ENTRY, NULL,
							  NULL, &cursor);
	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			   LDAP_XSTRING(wt_id2entry_delete)
			   ": open_cursor failed: %s (%d)\n",
			   wiredtiger_strerror(rc), rc, 0 );
		goto done;
	}
	cursor->set_key(cursor, e->e_id);
	rc = cursor->remove(cursor);
	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			   LDAP_XSTRING(wt_id2entry_delete)
			   ": remove failed: %s (%d)\n",
			   wiredtiger_strerror(rc), rc, 0 );
		goto done;
	}

done:
	if(cursor){
		cursor->close(cursor);
	}
	return rc;
}

int wt_id2entry( BackendDB *be,
				 WT_SESSION *session,
				 ID id,
				 Entry **ep ){
	int rc;
	WT_CURSOR *cursor = NULL;
	WT_ITEM item;
	EntryHeader eh;
	int eoff;
	Entry *e = NULL;

	rc = session->open_cursor(session, WT_TABLE_ID2ENTRY"(entry)", NULL,
							  NULL, &cursor);
	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			   LDAP_XSTRING(wt_id2entry)
			   ": open_cursor failed: %s (%d)\n",
			   wiredtiger_strerror(rc), rc, 0 );
		goto done;
	}

	cursor->set_key(cursor, id);
	rc = cursor->search(cursor);
	if ( rc ) {
		goto done;
	}

	cursor->get_value(cursor, &item);
	rc = wt_entry_header( &item,  &eh );
	eoff = eh.data - (char *)item.data;
	eh.bv.bv_len = eh.nvals * sizeof( struct berval ) + item.size;
	eh.bv.bv_val = ch_malloc( eh.bv.bv_len );
	memset(eh.bv.bv_val, 0xff, eh.bv.bv_len);
	eh.data = eh.bv.bv_val + eh.nvals * sizeof( struct berval );
	memcpy(eh.data, item.data, item.size);
	eh.data += eoff;
	rc = entry_decode( &eh, &e );
	if ( rc ) {
		Debug( LDAP_DEBUG_ANY,
			   LDAP_XSTRING(wt_id2entry)
			   ": entry decode error: %s (%d)\n",
			   rc, 0, 0 );
		goto done;
	}
	e->e_id = id;
	*ep = e;

done:
	if(cursor){
		cursor->close(cursor);
	}
	return rc;
}

int wt_entry_return(
	Entry *e
	)
{
	if ( !e ) {
		return 0;
	}

    /* Our entries are allocated in two blocks; the data comes from
	 * the db itself and the Entry structure and associated pointers
	 * are allocated in entry_decode. The db data pointer is saved
	 * in e_bv.
	 */
	if ( e->e_bv.bv_val ) {
#if 0
		/* See if the DNs were changed by modrdn */
		if( e->e_nname.bv_val < e->e_bv.bv_val || e->e_nname.bv_val >
			e->e_bv.bv_val + e->e_bv.bv_len ) {
			ch_free(e->e_name.bv_val);
			ch_free(e->e_nname.bv_val);
		}
#endif
		e->e_name.bv_val = NULL;
		e->e_nname.bv_val = NULL;
		/* In tool mode the e_bv buffer is realloc'd, leave it alone */
		if( !(slapMode & SLAP_TOOL_MODE) ) {
			free( e->e_bv.bv_val );
		}
		BER_BVZERO( &e->e_bv );
	}

	entry_free( e );
}

int wt_entry_release(
	Operation *op,
	Entry *e,
	int rw )
{
	return wt_entry_return( e );
}

/*
 * return LDAP_SUCCESS IFF we can retrieve the specified entry.
 */
int wt_entry_get(
	Operation *op,
	struct berval *ndn,
	ObjectClass *oc,
	AttributeDescription *at,
	int rw,
	Entry **ent )
{
	struct wt_info *wi = (struct wt_info *) op->o_bd->be_private;
	wt_ctx *wc;
	Entry *e = NULL;
	int	rc;
	const char *at_name = at ? at->ad_cname.bv_val : "(null)";

	Debug( LDAP_DEBUG_ARGS, LDAP_XSTRING(wt_entry_get)
		   ": ndn: \"%s\"\n", ndn->bv_val, 0, 0 );
	Debug( LDAP_DEBUG_ARGS, LDAP_XSTRING(wt_entry_get)
		   ": oc: \"%s\", at: \"%s\"\n",
		   oc ? oc->soc_cname.bv_val : "(null)", at_name, 0);

	wc = wt_ctx_get(op, wi);
	if( !wc ){
		Debug( LDAP_DEBUG_ANY, LDAP_XSTRING(wt_entry_get)
			   ": wt_ctx_get failed\n",
			   0, 0, 0 );
		return LDAP_OTHER;
	}
	rc = wt_dn2entry(op->o_bd, wc, ndn, &e);
	switch( rc ) {
	case 0:
		break;
	case WT_NOTFOUND:
		Debug( LDAP_DEBUG_ACL, LDAP_XSTRING(wt_entry_get)
			   ": cannot find entry: \"%s\"\n",
			   ndn->bv_val, 0, 0 );
		return LDAP_NO_SUCH_OBJECT;
	default:
		Debug( LDAP_DEBUG_ANY, LDAP_XSTRING(wt_entry_get)
			   ": wt_dn2entry failed rc=%d\n",
			   wiredtiger_strerror(rc), rc, 0 );
		rc = LDAP_OTHER;
	}

	Debug( LDAP_DEBUG_ACL, LDAP_XSTRING(wt_entry_get)
		   ": found entry: \"%s\"\n",
		   ndn->bv_val, 0, 0 );

	if ( oc && !is_entry_objectclass( e, oc, 0 )) {
		Debug( LDAP_DEBUG_ACL, LDAP_XSTRING(wt_entry_get)
			   ": failed to find objectClass %s\n",
			   oc->soc_cname.bv_val, 0, 0 );
		rc = LDAP_NO_SUCH_ATTRIBUTE;
		goto return_results;
	}

	/* NOTE: attr_find() or attrs_find()? */
	if ( at && attr_find( e->e_attrs, at ) == NULL ) {
		Debug( LDAP_DEBUG_ACL, LDAP_XSTRING(wt_entry_get)
			   ": failed to find attribute %s\n",
			   at->ad_cname.bv_val, 0, 0 );
		rc = LDAP_NO_SUCH_ATTRIBUTE;
		goto return_results;
	}

return_results:
	if( rc != LDAP_SUCCESS ) {
		wt_entry_return( e );
	}else{
		*ent = e;
	}

	Debug( LDAP_DEBUG_TRACE, LDAP_XSTRING(wt_entry_get)
		   ": rc=%d\n", rc, 0, 0 );

	return rc;
}

/*
 * Local variables:
 * indent-tabs-mode: t
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 */
