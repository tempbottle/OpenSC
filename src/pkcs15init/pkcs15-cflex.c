/*
 * Cryptoflex specific operation for PKCS #15 initialization
 *
 * Copyright (C) 2002  Juha Yrj�l� <juha.yrjola@iki.fi>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <opensc/opensc.h>
#include <opensc/cardctl.h>
#include <opensc/log.h>
#include "pkcs15-init.h"
#include "keycache.h"
#include "profile.h"

static void	invert_buf(u8 *dest, const u8 *src, size_t c);
static int	cflex_create_dummy_chvs(sc_profile_t *, sc_card_t *,
			sc_file_t *, int,
			sc_file_t **);
static void	cflex_delete_dummy_chvs(sc_profile_t *, sc_card_t *,
			int, sc_file_t **);
static int	cflex_create_pin_file(sc_profile_t *, sc_card_t *,
			sc_path_t *, int,
			const char *, size_t, int,
			const char *, size_t, int,
			sc_file_t **, int);
static int	cflex_create_empty_pin_file(sc_profile_t *, sc_card_t *,
			sc_path_t *, int, sc_file_t **);
static int	cflex_get_keyfiles(sc_profile_t *, sc_card_t *,
			const sc_path_t *, sc_file_t **, sc_file_t **);
static int	cflex_encode_private_key(struct sc_pkcs15_prkey_rsa *,
			u8 *, size_t *, int);
static int	cflex_encode_public_key(struct sc_pkcs15_prkey_rsa *,
			u8 *, size_t *, int);

static int
cflex_delete_file(sc_profile_t *profile, sc_card_t *card, sc_file_t *df)
{
        struct sc_path  path;
        struct sc_file  *parent;
        int             r = 0;
        /* Select the parent DF */
        path = df->path;
        path.len -= 2;
        r = sc_select_file(card, &path, &parent);
        if (r < 0)
                return r;

        r = sc_pkcs15init_authenticate(profile, card, parent, SC_AC_OP_DELETE);
        sc_file_free(parent);
        if (r < 0)
                return r;

	/* cryptoflex has no ERASE AC */
        memset(&path, 0, sizeof(path));
        path.type = SC_PATH_TYPE_FILE_ID;
        path.value[0] = df->id >> 8;
        path.value[1] = df->id & 0xFF;
        path.len = 2;

        card->ctx->suppress_errors++;
        r = sc_delete_file(card, &path);
        card->ctx->suppress_errors--;
        return r;
}

/*
 * Erase the card via rm
 */
static int cflex_erase_card(struct sc_profile *profile, struct sc_card *card)
{
	struct sc_file  *df = profile->df_info->file, *dir, *userpinfile;
	int             r;

	/* Delete EF(DIR). This may not be very nice
         * against other applications that use this file, but
         * extremely useful for testing :)
         * Note we need to delete if before the DF because we create
         * it *after* the DF. 
         * */
        if (sc_profile_get_file(profile, "DIR", &dir) >= 0) {
                r = cflex_delete_file(profile, card, dir);
                sc_file_free(dir);
                if (r < 0 && r != SC_ERROR_FILE_NOT_FOUND)
                        goto out;
        }

	r=cflex_delete_file(profile, card, df);

	/* If the user pin file isn't in a sub-DF of the pkcs15 DF, delete it */
	if (sc_profile_get_file(profile, "pinfile-1", &userpinfile) >= 0 &&
	    userpinfile->path.len <= profile->df_info->file->path.len + 2 &&
	    memcmp(userpinfile->path.value, profile->df_info->file->path.value,
	           userpinfile->path.len) != 0) {
           	r = cflex_delete_file(profile, card, userpinfile);
		sc_file_free(userpinfile);
	}


out:	/* Forget all cached keys, the pin files on card are all gone. */
	sc_keycache_forget_key(NULL, -1, -1);
        sc_free_apps(card);
        if (r == SC_ERROR_FILE_NOT_FOUND)
                r=0;
        return r;
}

/*
 * Create a DF
 */
static int
cflex_create_dir(sc_profile_t *profile, sc_card_t *card, sc_file_t *df)
{
	/* Create the application DF */
	return sc_pkcs15init_create_file(profile, card, df);
}

/*
 * Create a PIN domain (i.e. a sub-directory holding a user PIN)
 */
static int
cflex_create_domain(sc_profile_t *profile, sc_card_t *card,
		const sc_pkcs15_id_t *id, sc_file_t **ret)
{
	return sc_pkcs15_create_pin_domain(profile, card, id, ret);
}

/*
 * Select the PIN reference
 */
static int
cflex_select_pin_reference(sc_profile_t *profike, sc_card_t *card,
		sc_pkcs15_pin_info_t *pin_info)
{
	int	preferred;

	if (pin_info->flags & SC_PKCS15_PIN_FLAG_SO_PIN) {
		preferred = 2;
	} else {
		preferred = 1;
	}
	if (pin_info->reference <= preferred) {
		pin_info->reference = preferred;
		return 0;
	}

	if (pin_info->reference > 2)
		return SC_ERROR_INVALID_ARGUMENTS;

	/* Caller, please select a different PIN reference */
	return SC_ERROR_INVALID_PIN_REFERENCE;
}


/*
 * Create a new PIN inside a DF
 */
static int
cflex_create_pin(sc_profile_t *profile, sc_card_t *card, sc_file_t *df,
		sc_pkcs15_object_t *pin_obj,
		const unsigned char *pin, size_t pin_len,
		const unsigned char *puk, size_t puk_len)
{
	sc_pkcs15_pin_info_t *pin_info = (sc_pkcs15_pin_info_t *) pin_obj->data;
	sc_file_t	*dummies[2];
	int		ndummies, pin_type, puk_type, r;

	/* If the profile doesn't specify a reference for this PIN, guess */
	if (pin_info->flags & SC_PKCS15_PIN_FLAG_SO_PIN) {
		pin_type = SC_PKCS15INIT_SO_PIN;
		puk_type = SC_PKCS15INIT_SO_PUK;
		if (pin_info->reference != 2)
			return SC_ERROR_INVALID_ARGUMENTS;
	} else {
		pin_type = SC_PKCS15INIT_USER_PIN;
		puk_type = SC_PKCS15INIT_USER_PUK;
		if (pin_info->reference != 1)
			return SC_ERROR_INVALID_ARGUMENTS;
	}

	ndummies = cflex_create_dummy_chvs(profile, card,
				df, SC_AC_OP_CREATE,
				dummies);
	if (ndummies < 0)
		return ndummies;

	r = cflex_create_pin_file(profile, card, &df->path,
			pin_info->reference,
			pin, pin_len, sc_profile_get_pin_retries(profile, pin_type),
			puk, puk_len, sc_profile_get_pin_retries(profile, puk_type),
			NULL, 0);

	cflex_delete_dummy_chvs(profile, card, ndummies, dummies);
	return r;
}

/*
 * Create a new key file
 */
static int
cflex_create_key(sc_profile_t *profile, sc_card_t *card, sc_pkcs15_object_t *obj)
{
	sc_pkcs15_prkey_info_t *key_info = (sc_pkcs15_prkey_info_t *) obj->data;
	sc_file_t	*prkf = NULL, *pukf = NULL;
	size_t		size;
	int		r;

	if (obj->type != SC_PKCS15_TYPE_PRKEY_RSA) {
		sc_error(card->ctx, "Cryptoflex supports only RSA keys.");
		return SC_ERROR_NOT_SUPPORTED;
	}

	/* Get the public and private key file */
	r = cflex_get_keyfiles(profile, card,  &key_info->path, &prkf, &pukf);
	if (r < 0)
		return r;

	/* Adjust the file sizes, if necessary */
	switch (key_info->modulus_length) {
	case  512: size = 166; break;
	case  768: size = 246; break;
	case 1024: size = 326; break;
	case 2048: size = 646; break;
	default:
		sc_error(card->ctx, "Unsupported key size %u\n",
				key_info->modulus_length);
		r = SC_ERROR_INVALID_ARGUMENTS;
		goto out;
	}

	if (prkf->size < size)
		prkf->size = size;
	if (pukf->size < size + 4)
		pukf->size = size + 4;

	/* Now create the files */
	if ((r = sc_pkcs15init_create_file(profile, card, prkf)) < 0
	 || (r = sc_pkcs15init_create_file(profile, card, pukf)) < 0)
		goto out;

	key_info->key_reference = 0;

out:	if (prkf)
		sc_file_free(prkf);
	if (pukf)
		sc_file_free(pukf);
	return r;
}

/*
 * Generate key
 */
static int
cflex_generate_key(sc_profile_t *profile, sc_card_t *card,
			sc_pkcs15_object_t *obj,
			sc_pkcs15_pubkey_t *pubkey)
{
	struct sc_cardctl_cryptoflex_genkey_info args;
	sc_pkcs15_prkey_info_t *key_info = (sc_pkcs15_prkey_info_t *) obj->data;
	unsigned int	keybits;
	unsigned char	raw_pubkey[256];
	sc_file_t	*prkf = NULL, *pukf = NULL;
	int		r;

	if (obj->type != SC_PKCS15_TYPE_PRKEY_RSA) {
		sc_error(card->ctx, "Cryptoflex supports only RSA keys.");
		return SC_ERROR_NOT_SUPPORTED;
	}

	/* Get the public and private key file */
	r = cflex_get_keyfiles(profile, card, &key_info->path, &prkf, &pukf);
	if (r < 0)
		return r;

	/* Make sure we authenticate first */
	r = sc_pkcs15init_authenticate(profile, card, prkf, SC_AC_OP_CRYPTO);
	if (r < 0)
		goto out;

	keybits = key_info->modulus_length;

	/* Perform key generation */
	memset(&args, 0, sizeof(args));
	args.exponent = 0x10001;
	args.key_bits = keybits;
	args.key_num  = key_info->key_reference;
	r = sc_card_ctl(card, SC_CARDCTL_CRYPTOFLEX_GENERATE_KEY, &args);
	if (r < 0)
		goto out;

	/* extract public key */
	pubkey->algorithm = SC_ALGORITHM_RSA;
	pubkey->u.rsa.modulus.len   = keybits / 8;
	pubkey->u.rsa.modulus.data  = (u8 *) malloc(keybits / 8);
	pubkey->u.rsa.exponent.len  = 3;
	pubkey->u.rsa.exponent.data = (u8 *) malloc(3);
	memcpy(pubkey->u.rsa.exponent.data, "\x01\x00\x01", 3);
	if ((r = sc_select_file(card, &pukf->path, NULL)) < 0
	 || (r = sc_read_binary(card, 3, raw_pubkey, keybits / 8, 0)) < 0)
		goto out;

	invert_buf(pubkey->u.rsa.modulus.data, raw_pubkey, pubkey->u.rsa.modulus.len);

out:	if (pukf)
		sc_file_free(pukf);
	if (prkf)
		sc_file_free(prkf);
	return r;
}

/*
 * Store a private key
 */
static int
cflex_store_key(sc_profile_t *profile, sc_card_t *card,
		sc_pkcs15_object_t *obj,
		sc_pkcs15_prkey_t *key)
{
	sc_pkcs15_prkey_info_t *key_info = (sc_pkcs15_prkey_info_t *) obj->data;
	sc_file_t	*prkf, *pukf;
	unsigned char	keybuf[1024];
	size_t		size;
	int		r, key_num;

	if (obj->type != SC_PKCS15_TYPE_PRKEY_RSA) {
		sc_error(card->ctx, "Cryptoflex supports only RSA keys.");
		return SC_ERROR_NOT_SUPPORTED;
	}

	/* Get the public and private key file */
	r = cflex_get_keyfiles(profile, card, &key_info->path, &prkf, &pukf);
	if (r < 0)
		return r;

	key_num = key_info->key_reference + 1;

	size = sizeof(keybuf);
	if ((r = cflex_encode_private_key(&key->u.rsa, keybuf, &size, key_num)) < 0
	 || (r = sc_pkcs15init_update_file(profile, card, prkf, keybuf, size)) < 0)
		goto out;

	size = sizeof(keybuf);
	if ((r = cflex_encode_public_key(&key->u.rsa, keybuf, &size, key_num)) < 0
	 || (r = sc_pkcs15init_update_file(profile, card, pukf, keybuf, size)) < 0)
		goto out;

out:	sc_file_free(prkf);
	sc_file_free(pukf);
	return r;
}

/*
 * If an access condition references e.g. CHV1, but we don't have
 * a CHV1 file yet, create an unprotected dummy file in the MF.
 */
static int
cflex_create_dummy_chvs(sc_profile_t *profile, sc_card_t *card,
			sc_file_t *file, int op,
			sc_file_t **dummies)
{
	const sc_acl_entry_t *acl;
	int		r = 0, ndummies = 0;

	/* See if the DF is supposed to be PIN protected, and if
	 * it is, whether that CHV file actually exists. If it doesn't,
	 * create it.
	 */
	acl = sc_file_get_acl_entry(file, op);
	for (; acl; acl = acl->next) {
		sc_path_t	parent, ef;

		if (acl->method != SC_AC_CHV)
			continue;

		parent = file->path;
		parent.len -= 2;

		r = SC_ERROR_FILE_NOT_FOUND;
		while (parent.len >= 2 && r == SC_ERROR_FILE_NOT_FOUND) {
			ef = parent;
			ef.value[ef.len++] = acl->key_ref - 1;
			ef.value[ef.len++] = 0;
			parent.len -= 2;

			if (ef.len == parent.len
			 && !memcmp(ef.value, parent.value, ef.len))
				continue;

			card->ctx->suppress_errors++;
			r = sc_select_file(card, &ef, NULL);
			card->ctx->suppress_errors--;
		}

		/* If a valid EF(CHVx) was found, we're fine */
		if (r == 0)
			continue;
		if (r != SC_ERROR_FILE_NOT_FOUND)
			break;

		/* Create a CHV file in the MF */
		parent = file->path;
		parent.len = 2;
		r = cflex_create_empty_pin_file(profile, card, &parent,
				acl->key_ref, &dummies[ndummies]);
		if (r < 0)
			break;
		ndummies++;
	}

	if (r < 0) {
		cflex_delete_dummy_chvs(profile, card, ndummies, dummies);
		return r;
	}
	return ndummies;
}

static void
cflex_delete_dummy_chvs(sc_profile_t *profile, sc_card_t *card,
			int ndummies, sc_file_t **dummies)
{
	while (ndummies--) {
		cflex_delete_file(profile, card, dummies[ndummies]);
		sc_file_free(dummies[ndummies]);
	}
}

/*
 * Create a pin file
 */
static inline void
put_pin(sc_profile_t *profile, unsigned char *buf,
		const char *pin, size_t len, int retry)
{
	if (len > 8)
		len = 8;
	memset(buf, profile->pin_pad_char, 8);
	memcpy(buf, pin, len);
	buf[8] = retry;
	buf[9] = retry;
}

static int
cflex_create_pin_file(sc_profile_t *profile, sc_card_t *card,
			sc_path_t *df_path, int ref,
			const char *pin, size_t pin_len, int pin_tries,
			const char *puk, size_t puk_len, int puk_tries,
			sc_file_t **file_ret, int unprotected)
{
	unsigned char	buffer[23];
	sc_path_t	path;
	sc_file_t	*dummies[2], *file;
	int		r, ndummies;

	if (file_ret)
		*file_ret = NULL;

	/* Build the CHV path */
	path = *df_path;
	path.value[path.len++] = ref - 1;
	path.value[path.len++] = 0;

	/* See if the CHV already exists */
        card->ctx->suppress_errors++;
        r = sc_select_file(card, &path, NULL);
        card->ctx->suppress_errors--;
	if (r >= 0)
		return SC_ERROR_FILE_ALREADY_EXISTS;

	/* Get the file definition from the profile */
	if (sc_profile_get_file_by_path(profile, &path, &file) < 0
	 && sc_profile_get_file(profile, (ref == 1)? "CHV1" : "CHV2", &file) < 0
	 && sc_profile_get_file(profile, "CHV", &file) < 0) {
		sc_error(card->ctx, "profile does not define pin file ACLs\n");
		return SC_ERROR_FILE_NOT_FOUND;
	}

	file->path = path;
	file->size = 23;
	file->id = (ref == 1)? 0x0000 : 0x0100;

	if (unprotected) {
		sc_file_add_acl_entry(file, SC_AC_OP_UPDATE,
				SC_AC_NONE, SC_AC_KEY_REF_NONE);
	}


	/* Build the contents of the file */
	buffer[0] = buffer[1] = buffer[2] = 0xFF;
	put_pin(profile, buffer + 3, pin, pin_len, pin_tries);
	put_pin(profile, buffer + 13, puk, puk_len, puk_tries);

	/* For updating the file, create a dummy CHV files if
	 * necessary */
	ndummies = cflex_create_dummy_chvs(profile, card,
				file, SC_AC_OP_UPDATE,
				dummies);
	if (ndummies < 0) {
		sc_error(card->ctx,
			"Unable to create dummy CHV file: %s",
			sc_strerror(ndummies));
		return ndummies;
	}

	r = sc_pkcs15init_update_file(profile, card, file, buffer, 23);
	if (r >= 0)
		sc_keycache_put_key(df_path, SC_AC_CHV, ref, pin, pin_len);

	if (r < 0 || file_ret == NULL) {
		sc_file_free(file);
	} else {
		*file_ret = file;
	}

	/* Delete the dummy CHV files */
	cflex_delete_dummy_chvs(profile, card, ndummies, dummies);
	return r;
}

/*
 * Create a faux pin file
 */
static int
cflex_create_empty_pin_file(sc_profile_t *profile, sc_card_t *card,
			sc_path_t *path, int ref, sc_file_t **file_ret)
{
	int		r;

	*file_ret = NULL;
	r = cflex_create_pin_file(profile, card, path, ref,
			"0000", 4, 8,
			NULL, 0, 0,
			file_ret, 1);
	if (r == SC_ERROR_FILE_ALREADY_EXISTS)
		return 0;

	return r;
}

/*
 * Get private and public key file
 */
int
cflex_get_keyfiles(sc_profile_t *profile, sc_card_t *card,
			const sc_path_t *df_path,
			sc_file_t **prkf, sc_file_t **pukf)
{
	sc_path_t	path = *df_path;
	int		r;

	/* Get the private key file */
	r = sc_profile_get_file_by_path(profile, &path, prkf);
	if (r < 0) {
		sc_error(card->ctx, "Cannot find private key file info "
				"in profile (path=%s).",
				sc_print_path(&path));
		return r;
	}

	/* Get the public key file */
	path.len -= 2;
	sc_append_file_id(&path, 0x1012);
	r = sc_profile_get_file_by_path(profile, &path, pukf);
	if (r < 0) {
		sc_error(card->ctx, "Cannot find public key file info in profile.");
		sc_file_free(*prkf);
		return r;
	}

	return 0;
}

static void
invert_buf(u8 *dest, const u8 *src, size_t c)
{
	unsigned int i;

	for (i = 0; i < c; i++)
		dest[i] = src[c-1-i];
}

static int
bn2cf(sc_pkcs15_bignum_t *num, u8 *buf, size_t bufsize)
{
	size_t	len = num->len;

	if (len > bufsize)
		return SC_ERROR_INVALID_ARGUMENTS;

	invert_buf(buf, num->data, len);
	while (len < bufsize)
		buf[len++] = 0;
	return 0;
}

static int
cflex_encode_private_key(struct sc_pkcs15_prkey_rsa *rsa,
			u8 *key, size_t *keysize, int key_num)
{
        size_t base;
        int r;
        
        switch (rsa->modulus.len) {
	case  512 / 8:
	case  768 / 8:
	case 1024 / 8:
	case 2048 / 8:
                break;
	default:
		return SC_ERROR_INVALID_ARGUMENTS;
        }

	base = rsa->modulus.len / 2;
	if (*keysize < (5 * base + 6))
		return SC_ERROR_BUFFER_TOO_SMALL;
	*keysize = 5 * base + 6;

        *key++ = (5 * base + 3) >> 8;
        *key++ = (5 * base + 3) & 0xFF;
        *key++ = key_num;

	if ((r = bn2cf(&rsa->p,    key + 0 * base, base)) < 0
	 || (r = bn2cf(&rsa->q,    key + 1 * base, base)) < 0
	 || (r = bn2cf(&rsa->iqmp, key + 2 * base, base)) < 0
	 || (r = bn2cf(&rsa->dmp1, key + 3 * base, base)) < 0
	 || (r = bn2cf(&rsa->dmq1, key + 4 * base, base)) < 0)
		return r;

        key += 5 * base;
	*key++ = 0;
	*key++ = 0;
	*key++ = 0;
	
        return 0;
}

static int
cflex_encode_public_key(struct sc_pkcs15_prkey_rsa *rsa,
			u8 *key, size_t *keysize, int key_num)
{
        size_t base;
        int r;
        
        switch (rsa->modulus.len) {
	case  512 / 8:
	case  768 / 8:
	case 1024 / 8:
	case 2048 / 8:
                break;
	default:
		return SC_ERROR_INVALID_ARGUMENTS;
        }

	base = rsa->modulus.len / 2;
	if (*keysize < (5 * base + 10))
		return SC_ERROR_BUFFER_TOO_SMALL;
	*keysize = 5 * base + 10;
        
	memset(key, 0, *keysize);
        *key++ = (5 * base + 7) >> 8;
        *key++ = (5 * base + 7) & 0xFF;
        *key++ = key_num;

	/* Funny code - not sure why we do it this way:
	 * 
	 * Specs say:		We store:	(Length)
	 *  modulus		 modulus	(N bytes)
	 *  J0 Montgomery const	 0		(N/2 bytes)
	 *  H Montgomery const	 0		(N bytes)
	 *  exponent		 exponent	4
	 *
	 * 				--okir */
	if ((r = bn2cf(&rsa->modulus,  key + 0 * base, 2 * base)) < 0
	 || (r = bn2cf(&rsa->exponent, key + 5 * base, 4)) < 0)
		return r;

        return 0;
}

static struct sc_pkcs15init_operations sc_pkcs15init_cflex_operations;

struct sc_pkcs15init_operations *sc_pkcs15init_get_cflex_ops(void)
{
	sc_pkcs15init_cflex_operations.erase_card = cflex_erase_card;
	sc_pkcs15init_cflex_operations.create_dir = cflex_create_dir;
	sc_pkcs15init_cflex_operations.create_domain = cflex_create_domain;
	sc_pkcs15init_cflex_operations.select_pin_reference = cflex_select_pin_reference;
	sc_pkcs15init_cflex_operations.create_pin = cflex_create_pin;
	sc_pkcs15init_cflex_operations.create_key = cflex_create_key;
	sc_pkcs15init_cflex_operations.generate_key = cflex_generate_key;
	sc_pkcs15init_cflex_operations.store_key = cflex_store_key;

	return &sc_pkcs15init_cflex_operations;
}
