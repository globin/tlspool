/* tlspool/localid.c -- Map the keys of local identities to credentials */


#include <string.h>
#include <alloca.h>

#include <gnutls/gnutls.h>
#include <gnutls/abstract.h>

#include "manage.h"
#include "localid.h"


/*
 * Lookup local identities from a BDB database.  The identities take the
 * form of a NAI, and are the keys for a key-values lookup.  The outcome
 * may offer multiple values, each representing an identity.  The general
 * structure of a value is:
 *
 * - 4 netbytes, a flags field for local identity management (see LID_xxx below)
 * - NUL-terminated string with a pkcs11 URI [ draft-pechanec-pkcs11uri ]
 * - Binary string holding the identity in binary form
 *
 * There may be prefixes for generic management, but these are not made
 * available to this layer.
 */



/* Interpret the credentials structure found in dbh_localid.
 * This comes down to splitting the (data,size) structure into fields:
 *  - a 32-bit flags field
 *  - a char * sharing the PKCS #11 private key location
 *  - a (data,size) structure for the public credential
 * The function returns non-zero on success (zero indicates syntax error).
 */
int dbcred_interpret (DBT *creddata, uint32_t *flags, char **p11priv, uint8_t **pubdata, int *pubdatalen) {
	int p11privlen;
	if (creddata->size <= 4) {
		return 0;
	}
	*flags = ntohl (* (uint32_t *) creddata->data);
	*p11priv = ((char *) creddata->data) + 4;
	p11privlen = strnlen (*p11priv, creddata->size - 4);
	if (p11privlen == creddata->size - 4) {
		return 0;
	}
#ifdef TODO_PKCS11_ADDED
	if (strncmp (*p11priv, "pkcs11:", 7) != 0) {
		return 0;
	}
#endif
	*pubdata    = ((uint8_t *) creddata->data) + 4 + p11privlen + 1;
	*pubdatalen =              creddata->size  - 4 - p11privlen - 1;
	if (*pubdatalen < 20) {
		// Unbelievably short certificate (arbitrary sanity limit 20)
		return 0;
	}
	return 1;
}


/* Create an iterator for a given localid value.  Use keys from dhb_lid.
 * The first value is delivered; continue with dbcred_iterate_next().
 *
 * The cursor must have been opened on dbh_localid within the desired
 * transaction context; the caller must close it after iteration.
 *
 * The value returned is only non-zero if a value was setup.
 * The DB_NOTFOUND value indicates that the key was not found.
 */
int dbcred_iterate_from_localid (DBC *cursor, char *localid, DBT *keydata, DBT *creddata) {
	int err = 0;
	memset (keydata, 0, sizeof (*keydata));
	keydata->data = localid;
	keydata->size = strlen (localid);
	err = cursor->get (cursor, keydata, creddata, DB_SET);
	return err;
}


/* Construct an iterator for a given remoteid value.
 * Apply stepwise generalisation to selectors to find the most concrete match.
 * The first value is delivered; continue with dbcred_iterate_next().
 *
 * The remoteid value is stored in discpatn, forming the initial disclosure
 * pattern, which is the full address.  Upon return, the actual matching
 * pattern is filled in; it is never longer, so the allocated memory is
 * never overwritten; at worst it is not fully utilised due to a lower size.
 *
 * The started iteration is a nested iteration over dbh_disclose for the
 * given remoteid, and inside that an iteration over dbh_localid for the
 * localid values that this gave.  This means that two cursors are needed,
 * both here and in the subsequent dbcred_iterate_next() calls.
 *
 * The cursors crs_disclose and crs_localid must have been opened on
 * dbh_disclose and dbh_localid within the desired transaction context;
 * the caller must close them after iteration.
 *
 * The value returned is zero if a value was setup; otherwise an error code.
 * The DB_NOTFOUND value indicates that no selector matching the remoteid
 * was found in dbh_disclose.
 */
int dbcred_iterate_from_remoteid (DBC *crs_disclose, DBC *crs_localid, DBT *discpatn, DBT *keydata, DBT *creddata) {
	int err = 0;
	char *stable_rid = NULL;
	donai_t remote_donai;
	donai_t remote_selector;
	char *selector_text;
	int more;
	stable_rid = alloca (discpatn->size);
	memcpy (stable_rid, discpatn->data, discpatn->size);
	remote_donai = donai_from_stable_string (stable_rid, discpatn->size);
	more = selector_iterate_init (&remote_selector, &remote_donai);
	while (more) {
		int fnd;
		discpatn->size = donai_iterate_memput (discpatn->data, &remote_selector);
		fnd = crs_disclose->get (crs_disclose, discpatn, keydata, DB_SET);
fprintf (stderr, "DEBUG: crs_disclose->get(%.*s) returned %s\n", discpatn->size, discpatn->data, db_strerror (fnd));
		if (fnd == 0) {
			// Got the selector pattern!
			// Now continue, even when no localids will work.
			err = err || crs_localid->get (crs_localid, keydata, creddata, DB_SET);
			return err;
		} else if (fnd != DB_NOTFOUND) {
			err = fnd;
			break;
		}
		more = selector_iterate_next (&remote_selector);
	}
	// Ended here with nothing more to find
	return DB_NOTFOUND;
}

/* Move an iterator to the next credential data value.  When done, the value
 * returned should be DB_NOTFOUND.
 *
 * The outer cursor (for dbh_disclose) is optional, and is only used when
 * the prior call was from dbcred_iterate_from_remoteid().
 *
 * The optional discpatn must be supplied only when dbh_disclose is provided.
 * It holds the key value for the dbh_disclose outer cursor.
 *
 * The keydata will be filled with the intermediate key when dbh_disclose is
 * provided.  It is also used to match the next record with the current one.
 *
 * The value returned is zero if a value was setup; otherwise an error code.
 * The DB_NOTFOUND value indicates that no further duplicate was not found.
 */
int dbcred_iterate_next (DBC *opt_crs_disclose, DBC *crs_localid, DBT *opt_discpatn, DBT *keydata, DBT *creddata) {
	int err;
	err = crs_localid->get (crs_localid, keydata, creddata, DB_NEXT_DUP);
	if (err != DB_NOTFOUND) {
		return err;
	}
	// Inner loop ended in DB_NOTFOUND, optionally continue in outer loop
	if ((opt_crs_disclose != NULL) && (opt_discpatn != NULL)) {
		while (err == DB_NOTFOUND) {
			err = opt_crs_disclose->get (opt_crs_disclose, opt_discpatn, keydata, DB_NEXT_DUP);
			if (err == DB_NOTFOUND) {
				return err;
			}
			err = crs_localid->get (crs_localid, keydata, creddata, DB_SET);
		}
	}
	return err;
}



/* Iterate over selector values that would generalise the donai.  The
 * selector_t shares data from the donai, so it allocates no internal
 * storage and so it can be dropped at any time during the iteration.
 * Meanwhile, the donai must not drop storage before iteration stops.
 *
 * The value returned is only non-zero if a value was setup.
 */
int selector_iterate_init (selector_t *iterator, donai_t *donai) {
	//
	// If the user name is not NULL but empty, bail out in horror
	if ((donai->user != NULL) && (donai->userlen == 0)) {
		return 0;
	}
	//
	// If the domain name is empty or NULL, bail out in horror
	if ((donai->domain == NULL) || (donai->domlen == 0)) {
		return 0;
	}
	//
	// The first and most concrete pattern is the donai itself
	memcpy (iterator, donai, sizeof (*iterator));
	return 1;
}

int selector_iterate_next (selector_t *iterator) {
	int skip;
	//
	// If the user name is not NULL but empty, bail out in horror
	if ((iterator->user != NULL) && (iterator->userlen == 0)) {
		return 0;
	}
	//
	// If the domain name is empty or NULL, bail out in horror
	if ((iterator->domain == NULL) || (iterator->domlen == 0)) {
		return 0;
	}
	//
	// If there is a user component and it is non-empty, make it empty
	// If it was empty, permit it to become non-empty again, and continue
	if (iterator->user) {
		if (iterator->userlen > 0) {
			iterator->userlen = -iterator->userlen;
			return 1;
		}
		iterator->userlen = -iterator->userlen;
	}
	//
	// If the domain is a single dot, we're done
	if ((iterator->domlen == 1) && (*iterator->domain == '.')) {
		return 0;
	}
	//
	// Replace the domain (known >= 1 chars) with the next dot's domain
	skip = 1;
	while ((skip < iterator->domlen) && (iterator->domain [skip] != '.')) {
		skip++;
	}
	if (skip == iterator->domlen) {
		iterator->domain = ".";		// Last resort domain
		iterator->domlen = 1;
	} else {
		iterator->domain += skip;
		iterator->domlen -= skip;
	}
	return 1;
}


/* Check if a selector is a pattern that matches the given donai value.
 * The value returned is non-zero for a match, zero for a non-match.
 */
int donai_matches_selector (donai_t *donai, selector_t *pattern) {
	int extra;
	//
	// Bail out in horror on misconfigurations
	if ((donai->user != NULL) && (donai->userlen == 0)) {
		return 0;
	}
	if ((donai  ->domain == NULL) || (donai  ->domlen == 0)) {
		return 0;
	}
	if ((pattern->domain == NULL) || (pattern->domlen == 0)) {
		return 0;
	}
	//
	// User name handling first
	if (pattern->user) {
		//
		// Pattern has a user?  Then request a user in the donai too
		if (donai->user == NULL) {
			return 0;
		}
		//
		// Non-empty user in pattern?  Then match everything
		if (*pattern->user) {
			if (pattern->userlen > 0) {
				if (donai->userlen != pattern->userlen) {
					return 0;
				}
				if (memcmp (donai->user, pattern->user, donai->userlen) != 0) {
					return 0;
				}
			}
		}
	} else {
		//
		// Pattern without user, then donai may not have one either
		if (donai->user != NULL) {
			return 0;
		}
	}
	//
	// Domain name handling second
	if (*pattern->domain == '.') {
		extra = donai->domlen - pattern->domlen;
		if (extra < 0) {
			//
			// No good having a longer pattern than a donai.domain
			return 0;
		}
	} else {
		extra = 0;
	}
	return (memcmp (donai->domain + extra, pattern->domain, pattern->domlen) == 0);
}


/* Fill a donai structure from a stable string. The donai will share parts
 * of the string.
 */
donai_t donai_from_stable_string (char *stable, int stablelen) {
	donai_t retval;
	retval.userlen = stablelen - 1;
	while (retval.userlen > 0) {
		if (stable [retval.userlen] == '@') {
			break;
		}
		retval.userlen--;
	}
	if (stable [retval.userlen] == '@') {
		retval.user = stable;
		retval.domain = stable + (retval.userlen + 1);
		retval.domlen = stablelen - 1 - retval.userlen;
	} else {
		retval.user = NULL;
		retval.domain = stable;
		retval.domlen = stablelen;
	}
	return retval;
}

/* Print a donai or iterated selector to the given text buffer.  The
 * text will be precisely the same as the originally parsed text.  An
 * iterator may deliver values that are shorter, not longer.  The value
 * returned is the number of bytes written.  No trailing NUL character
 * will be written.
 */
int donai_iterate_memput (char *selector_text, donai_t *iterator) {
	int len = 0;
	if (iterator->user != NULL) {
		if (iterator->userlen > 0) {
			memcpy (selector_text, iterator->user, iterator->userlen);
			len += iterator->userlen;
		}
		selector_text [len++] = '@';
	}
	memcpy (selector_text + len, iterator->domain, iterator->domlen);
	len += iterator->domlen;
	return len;
}
