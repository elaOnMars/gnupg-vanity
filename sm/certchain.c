/* certchain.c - certificate chain validation
 * Copyright (C) 2001, 2002, 2003, 2004, 2005,
 *               2006 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,
 * USA.
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h> 
#include <time.h>
#include <stdarg.h>
#include <assert.h>

#define JNLIB_NEED_LOG_LOGV /* We need log_logv. */

#include "gpgsm.h"
#include <gcrypt.h>
#include <ksba.h>

#include "keydb.h"
#include "../kbx/keybox.h" /* for KEYBOX_FLAG_* */
#include "i18n.h"


/* Object to keep track of certain root certificates. */
struct marktrusted_info_s
{
  struct marktrusted_info_s *next;
  unsigned char fpr[20];
};
static struct marktrusted_info_s *marktrusted_info;


static int get_regtp_ca_info (ksba_cert_t cert, int *chainlen);


/* This function returns true if we already asked during this session
   whether the root certificate CERT shall be marked as trusted.  */
static int
already_asked_marktrusted (ksba_cert_t cert)
{
  unsigned char fpr[20];
  struct marktrusted_info_s *r;

  gpgsm_get_fingerprint (cert, GCRY_MD_SHA1, fpr, NULL);
  /* No context switches in the loop! */
  for (r=marktrusted_info; r; r= r->next)
    if (!memcmp (r->fpr, fpr, 20))
      return 1;
  return 0;
}

/* Flag certificate CERT as already asked whether it shall be marked
   as trusted.  */
static void
set_already_asked_marktrusted (ksba_cert_t cert)
{
 unsigned char fpr[20];
 struct marktrusted_info_s *r;

 gpgsm_get_fingerprint (cert, GCRY_MD_SHA1, fpr, NULL);
 for (r=marktrusted_info; r; r= r->next)
   if (!memcmp (r->fpr, fpr, 20))
     return; /* Already marked. */
 r = xtrycalloc (1, sizeof *r);
 if (!r)
   return;
 memcpy (r->fpr, fpr, 20);
 r->next = marktrusted_info;
 marktrusted_info = r;
}

/* If LISTMODE is true, print FORMAT using LISTMODE to FP.  If
   LISTMODE is false, use the string to print an log_info or, if
   IS_ERROR is true, and log_error. */
static void
do_list (int is_error, int listmode, FILE *fp, const char *format, ...)
{
  va_list arg_ptr;

  va_start (arg_ptr, format) ;
  if (listmode)
    {
      if (fp)
        {
          fputs ("  [", fp);
          vfprintf (fp, format, arg_ptr);
          fputs ("]\n", fp);
        }
    }
  else
    {
      log_logv (is_error? JNLIB_LOG_ERROR: JNLIB_LOG_INFO, format, arg_ptr);
      log_printf ("\n");
    }
  va_end (arg_ptr);
}

/* Return 0 if A and B are equal. */
static int
compare_certs (ksba_cert_t a, ksba_cert_t b)
{
  const unsigned char *img_a, *img_b;
  size_t len_a, len_b;

  img_a = ksba_cert_get_image (a, &len_a);
  if (!img_a)
    return 1;
  img_b = ksba_cert_get_image (b, &len_b);
  if (!img_b)
    return 1;
  return !(len_a == len_b && !memcmp (img_a, img_b, len_a));
}


static int
unknown_criticals (ksba_cert_t cert, int listmode, FILE *fp)
{
  static const char *known[] = {
    "2.5.29.15", /* keyUsage */
    "2.5.29.19", /* basic Constraints */
    "2.5.29.32", /* certificatePolicies */
    "2.5.29.37", /* extendedKeyUsage - handled by certlist.c */
    NULL
  };
  int rc = 0, i, idx, crit;
  const char *oid;
  gpg_error_t err;

  for (idx=0; !(err=ksba_cert_get_extension (cert, idx,
                                             &oid, &crit, NULL, NULL));idx++)
    {
      if (!crit)
        continue;
      for (i=0; known[i] && strcmp (known[i],oid); i++)
        ;
      if (!known[i])
        {
          do_list (1, listmode, fp,
                   _("critical certificate extension %s is not supported"),
                   oid);
          rc = gpg_error (GPG_ERR_UNSUPPORTED_CERT);
        }
    }
  /* We ignore the error codes EOF as well as no-value. The later will
     occur for certificates with no extensions at all. */
  if (err
      && gpg_err_code (err) != GPG_ERR_EOF
      && gpg_err_code (err) != GPG_ERR_NO_VALUE)
    rc = err;

  return rc;
}


/* Check whether CERT is an allowed certificate.  This requires that
   CERT matches all requirements for such a CA, i.e. the
   BasicConstraints extension.  The function returns 0 on success and
   the awlloed length of the chain at CHAINLEN. */
static int
allowed_ca (ksba_cert_t cert, int *chainlen, int listmode, FILE *fp)
{
  gpg_error_t err;
  int flag;

  err = ksba_cert_is_ca (cert, &flag, chainlen);
  if (err)
    return err;
  if (!flag)
    {
      if (get_regtp_ca_info (cert, chainlen))
        {
          /* Note that dirmngr takes a different way to cope with such
             certs. */
          return 0; /* RegTP issued certificate. */
        }

      do_list (1, listmode, fp,_("issuer certificate is not marked as a CA"));
      return gpg_error (GPG_ERR_BAD_CA_CERT);
    }
  return 0;
}


static int
check_cert_policy (ksba_cert_t cert, int listmode, FILE *fplist)
{
  gpg_error_t err;
  char *policies;
  FILE *fp;
  int any_critical;

  err = ksba_cert_get_cert_policies (cert, &policies);
  if (gpg_err_code (err) == GPG_ERR_NO_DATA 
      || gpg_err_code (err) == GPG_ERR_NO_VALUE)
    return 0; /* No policy given. */
  if (err)
    return err;

  /* STRING is a line delimited list of certifiate policies as stored
     in the certificate.  The line itself is colon delimited where the
     first field is the OID of the policy and the second field either
     N or C for normal or critical extension */

  if (opt.verbose > 1 && !listmode)
    log_info ("certificate's policy list: %s\n", policies);

  /* The check is very minimal but won't give false positives */
  any_critical = !!strstr (policies, ":C");

  if (!opt.policy_file)
    { 
      xfree (policies);
      if (any_critical)
        {
          do_list (1, listmode, fplist,
                   _("critical marked policy without configured policies"));
          return gpg_error (GPG_ERR_NO_POLICY_MATCH);
        }
      return 0;
    }

  fp = fopen (opt.policy_file, "r");
  if (!fp)
    {
      if (opt.verbose || errno != ENOENT)
        log_info (_("failed to open `%s': %s\n"),
                  opt.policy_file, strerror (errno));
      xfree (policies);
      /* With no critical policies this is only a warning */
      if (!any_critical)
        {
          do_list (0, listmode, fplist,
                   _("note: non-critical certificate policy not allowed"));
          return 0;
        }
      do_list (1, listmode, fplist,
               _("certificate policy not allowed"));
      return gpg_error (GPG_ERR_NO_POLICY_MATCH);
    }

  for (;;) 
    {
      int c;
      char *p, line[256];
      char *haystack, *allowed;

      /* read line */
      do
        {
          if (!fgets (line, DIM(line)-1, fp) )
            {
              gpg_error_t tmperr = gpg_error (gpg_err_code_from_errno (errno));

              xfree (policies);
              if (feof (fp))
                {
                  fclose (fp);
                  /* With no critical policies this is only a warning */
                  if (!any_critical)
                    {
                      do_list (0, listmode, fplist,
                     _("note: non-critical certificate policy not allowed"));
                      return 0;
                    }
                  do_list (1, listmode, fplist,
                           _("certificate policy not allowed"));
                  return gpg_error (GPG_ERR_NO_POLICY_MATCH);
                }
              fclose (fp);
              return tmperr;
            }
      
          if (!*line || line[strlen(line)-1] != '\n')
            {
              /* eat until end of line */
              while ( (c=getc (fp)) != EOF && c != '\n')
                ;
              fclose (fp);
              xfree (policies);
              return gpg_error (*line? GPG_ERR_LINE_TOO_LONG
                                     : GPG_ERR_INCOMPLETE_LINE);
            }
          
          /* Allow for empty lines and spaces */
          for (p=line; spacep (p); p++)
            ;
        }
      while (!*p || *p == '\n' || *p == '#');
  
      /* parse line */
      for (allowed=line; spacep (allowed); allowed++)
        ;
      p = strpbrk (allowed, " :\n");
      if (!*p || p == allowed)
        {
          fclose (fp);
          xfree (policies);
          return gpg_error (GPG_ERR_CONFIGURATION);
        }
      *p = 0; /* strip the rest of the line */
      /* See whether we find ALLOWED (which is an OID) in POLICIES */
      for (haystack=policies; (p=strstr (haystack, allowed)); haystack = p+1)
        {
          if ( !(p == policies || p[-1] == '\n') )
            continue; /* Does not match the begin of a line. */
          if (p[strlen (allowed)] != ':')
            continue; /* The length does not match. */
          /* Yep - it does match so return okay. */
          fclose (fp);
          xfree (policies);
          return 0;
        }
    }
}


/* Helper function for find_up.  This resets the key handle and search
   for an issuer ISSUER with a subjectKeyIdentifier of KEYID.  Returns
   0 obn success or -1 when not found. */
static int
find_up_search_by_keyid (KEYDB_HANDLE kh,
                         const char *issuer, ksba_sexp_t keyid)
{
  int rc;
  ksba_cert_t cert = NULL;
  ksba_sexp_t subj = NULL;

  keydb_search_reset (kh);
  while (!(rc = keydb_search_subject (kh, issuer)))
    {
      ksba_cert_release (cert); cert = NULL;
      rc = keydb_get_cert (kh, &cert);
      if (rc)
        {
          log_error ("keydb_get_cert() failed: rc=%d\n", rc);
          rc = -1;
          break;
        }
      xfree (subj);
      if (!ksba_cert_get_subj_key_id (cert, NULL, &subj))
        {
          if (!cmp_simple_canon_sexp (keyid, subj))
            break; /* Found matching cert. */
        }
    }
  
  ksba_cert_release (cert);
  xfree (subj);
  return rc? -1:0;
}


static void
find_up_store_certs_cb (void *cb_value, ksba_cert_t cert)
{
  if (keydb_store_cert (cert, 1, NULL))
    log_error ("error storing issuer certificate as ephemeral\n");
  ++*(int*)cb_value;
}


/* Helper for find_up().  Locate the certificate for ISSUER using an
   external lookup.  KH is the keydb context we are currently using.
   On success 0 is returned and the certificate may be retrieved from
   the keydb using keydb_get_cert().  KEYID is the keyIdentifier from
   the AKI or NULL. */
static int
find_up_external (KEYDB_HANDLE kh, const char *issuer, ksba_sexp_t keyid)
{
  int rc;
  strlist_t names = NULL;
  int count = 0;
  char *pattern;
  const char *s;
      
  if (opt.verbose)
    log_info (_("looking up issuer at external location\n"));
  /* The DIRMNGR process is confused about unknown attributes.  As a
     quick and ugly hack we locate the CN and use the issuer string
     starting at this attribite.  Fixme: we should have far better
     parsing in the dirmngr. */
  s = strstr (issuer, "CN=");
  if (!s || s == issuer || s[-1] != ',')
    s = issuer;

  pattern = xtrymalloc (strlen (s)+2);
  if (!pattern)
    return gpg_error_from_syserror ();
  strcpy (stpcpy (pattern, "/"), s);
  add_to_strlist (&names, pattern);
  xfree (pattern);

  rc = gpgsm_dirmngr_lookup (NULL, names, find_up_store_certs_cb, &count);
  free_strlist (names);

  if (opt.verbose)
    log_info (_("number of issuers matching: %d\n"), count);
  if (rc) 
    {
      log_error ("external key lookup failed: %s\n", gpg_strerror (rc));
      rc = -1;
    }
  else if (!count)
    rc = -1;
  else
    {
      int old;
      /* The issuers are currently stored in the ephemeral key DB, so
         we temporary switch to ephemeral mode. */
      old = keydb_set_ephemeral (kh, 1);
      if (keyid)
        rc = find_up_search_by_keyid (kh, issuer, keyid);
      else
        {
          keydb_search_reset (kh);
          rc = keydb_search_subject (kh, issuer);
        }
      keydb_set_ephemeral (kh, old);
    }
  return rc;
}


/* Locate issuing certificate for CERT. ISSUER is the name of the
   issuer used as a fallback if the other methods don't work.  If
   FIND_NEXT is true, the function shall return the next possible
   issuer.  The certificate itself is not directly returned but a
   keydb_get_cert on the keyDb context KH will return it.  Returns 0
   on success, -1 if not found or an error code.  */
static int
find_up (KEYDB_HANDLE kh, ksba_cert_t cert, const char *issuer, int find_next)
{
  ksba_name_t authid;
  ksba_sexp_t authidno;
  ksba_sexp_t keyid;
  int rc = -1;

  if (!ksba_cert_get_auth_key_id (cert, &keyid, &authid, &authidno))
    {
      const char *s = ksba_name_enum (authid, 0);
      if (s && *authidno)
        {
          rc = keydb_search_issuer_sn (kh, s, authidno);
          if (rc)
              keydb_search_reset (kh);
          
          /* In case of an error try the ephemeral DB.  We can't do
             that in find_next mode because we can't keep the search
             state then. */
          if (rc == -1 && !find_next)
            { 
              int old = keydb_set_ephemeral (kh, 1);
              if (!old)
                {
                  rc = keydb_search_issuer_sn (kh, s, authidno);
                  if (rc)
                    keydb_search_reset (kh);
                }
              keydb_set_ephemeral (kh, old);
            }

        }

      if (rc == -1 && keyid && !find_next)
        {
          /* Not found by AIK.issuer_sn.  Lets try the AIY.ki
             instead. Loop over all certificates with that issuer as
             subject and stop for the one with a matching
             subjectKeyIdentifier. */
          rc = find_up_search_by_keyid (kh, issuer, keyid);
          if (rc)
            {
              int old = keydb_set_ephemeral (kh, 1);
              if (!old)
                rc = find_up_search_by_keyid (kh, issuer, keyid);
              keydb_set_ephemeral (kh, old);
            }
          if (rc) 
            rc = -1; /* Need to make sure to have this error code. */
        }

      /* If we still didn't found it, try an external lookup.  */
      if (rc == -1 && opt.auto_issuer_key_retrieve && !find_next)
        rc = find_up_external (kh, issuer, keyid);

      /* Print a note so that the user does not feel too helpless when
         an issuer certificate was found and gpgsm prints BAD
         signature because it is not the correct one. */
      if (rc == -1)
        {
          log_info ("%sissuer certificate ", find_next?"next ":"");
          if (keyid)
            {
              log_printf ("{");
              gpgsm_dump_serial (keyid);
              log_printf ("} ");
            }
          if (authidno)
            {
              log_printf ("(#");
              gpgsm_dump_serial (authidno);
              log_printf ("/");
              gpgsm_dump_string (s);
              log_printf (") ");
            }
          log_printf ("not found using authorityKeyIdentifier\n");
        }
      else if (rc)
        log_error ("failed to find authorityKeyIdentifier: rc=%d\n", rc);
      xfree (keyid);
      ksba_name_release (authid);
      xfree (authidno);
    }
  
  if (rc) /* Not found via authorithyKeyIdentifier, try regular issuer name. */
    rc = keydb_search_subject (kh, issuer);
  if (rc == -1 && !find_next)
    {
      /* Not found, lets see whether we have one in the ephemeral key DB. */
      int old = keydb_set_ephemeral (kh, 1);
      if (!old)
        {
          keydb_search_reset (kh);
          rc = keydb_search_subject (kh, issuer);
        }
      keydb_set_ephemeral (kh, old);
    }

  /* Still not found.  If enabled, try an external lookup.  */
  if (rc == -1 && opt.auto_issuer_key_retrieve && !find_next)
    rc = find_up_external (kh, issuer, NULL);

  return rc;
}


/* Return the next certificate up in the chain starting at START.
   Returns -1 when there are no more certificates. */
int
gpgsm_walk_cert_chain (ksba_cert_t start, ksba_cert_t *r_next)
{
  int rc = 0; 
  char *issuer = NULL;
  char *subject = NULL;
  KEYDB_HANDLE kh = keydb_new (0);

  *r_next = NULL;
  if (!kh)
    {
      log_error (_("failed to allocated keyDB handle\n"));
      rc = gpg_error (GPG_ERR_GENERAL);
      goto leave;
    }

  issuer = ksba_cert_get_issuer (start, 0);
  subject = ksba_cert_get_subject (start, 0);
  if (!issuer)
    {
      log_error ("no issuer found in certificate\n");
      rc = gpg_error (GPG_ERR_BAD_CERT);
      goto leave;
    }
  if (!subject)
    {
      log_error ("no subject found in certificate\n");
      rc = gpg_error (GPG_ERR_BAD_CERT);
      goto leave;
    }

  if (!strcmp (issuer, subject))
    {
      rc = -1; /* we are at the root */
      goto leave; 
    }

  rc = find_up (kh, start, issuer, 0);
  if (rc)
    {
      /* it is quite common not to have a certificate, so better don't
         print an error here */
      if (rc != -1 && opt.verbose > 1)
        log_error ("failed to find issuer's certificate: rc=%d\n", rc);
      rc = gpg_error (GPG_ERR_MISSING_CERT);
      goto leave;
    }

  rc = keydb_get_cert (kh, r_next);
  if (rc)
    {
      log_error ("keydb_get_cert() failed: rc=%d\n", rc);
      rc = gpg_error (GPG_ERR_GENERAL);
    }

 leave:
  xfree (issuer);
  xfree (subject);
  keydb_release (kh); 
  return rc;
}


/* Check whether the CERT is a root certificate.  Returns True if this
   is the case. */
int
gpgsm_is_root_cert (ksba_cert_t cert)
{
  char *issuer;
  char *subject;
  int yes;

  issuer = ksba_cert_get_issuer (cert, 0);
  subject = ksba_cert_get_subject (cert, 0);
  yes = (issuer && subject && !strcmp (issuer, subject));
  xfree (issuer);
  xfree (subject);
  return yes;
}


/* This is a helper for gpgsm_validate_chain. */
static gpg_error_t 
is_cert_still_valid (ctrl_t ctrl, int lm, FILE *fp,
                     ksba_cert_t subject_cert, ksba_cert_t issuer_cert,
                     int *any_revoked, int *any_no_crl, int *any_crl_too_old)
{
  if (!opt.no_crl_check || ctrl->use_ocsp)
    {
      gpg_error_t err;

      err = gpgsm_dirmngr_isvalid (ctrl,
                                   subject_cert, issuer_cert, ctrl->use_ocsp);
      if (err)
        {
          /* Fixme: We should change the wording because we may
             have used OCSP. */
          if (!lm)
            gpgsm_cert_log_name (NULL, subject_cert);
          switch (gpg_err_code (err))
            {
            case GPG_ERR_CERT_REVOKED:
              do_list (1, lm, fp, _("certificate has been revoked"));
              *any_revoked = 1;
              /* Store that in the keybox so that key listings are
                 able to return the revoked flag.  We don't care
                 about error, though. */
              keydb_set_cert_flags (subject_cert, KEYBOX_FLAG_VALIDITY, 0,
                                    VALIDITY_REVOKED);
              break;
            case GPG_ERR_NO_CRL_KNOWN:
              do_list (1, lm, fp, _("no CRL found for certificate"));
              *any_no_crl = 1;
              break;
            case GPG_ERR_CRL_TOO_OLD:
              do_list (1, lm, fp, _("the available CRL is too old"));
              if (!lm)
                log_info (_("please make sure that the "
                            "\"dirmngr\" is properly installed\n"));
              *any_crl_too_old = 1;
              break;
            default:
              do_list (1, lm, fp, _("checking the CRL failed: %s"),
                       gpg_strerror (err));
              return err;
            }
        }
    }
  return 0;
}



/* Validate a chain and optionally return the nearest expiration time
   in R_EXPTIME. With LISTMODE set to 1 a special listmode is
   activated where only information about the certificate is printed
   to FP and no output is send to the usual log stream. 

   Defined flag bits: 0 - do not do any dirmngr isvalid checks.
*/
int
gpgsm_validate_chain (ctrl_t ctrl, ksba_cert_t cert, ksba_isotime_t r_exptime,
                      int listmode, FILE *fp, unsigned int flags)
{
  int rc = 0, depth = 0, maxdepth;
  char *issuer = NULL;
  char *subject = NULL;
  KEYDB_HANDLE kh = NULL;
  ksba_cert_t subject_cert = NULL, issuer_cert = NULL;
  ksba_isotime_t current_time;
  ksba_isotime_t exptime;
  int any_expired = 0;
  int any_revoked = 0;
  int any_no_crl = 0;
  int any_crl_too_old = 0;
  int any_no_policy_match = 0;
  int is_qualified = -1; /* Indicates whether the certificate stems
                            from a qualified root certificate.
                            -1 = unknown, 0 = no, 1 = yes. */
  int lm = listmode;

  gnupg_get_isotime (current_time);
  if (r_exptime)
    *r_exptime = 0;
  *exptime = 0;

  if (opt.no_chain_validation && !listmode)
    {
      log_info ("WARNING: bypassing certificate chain validation\n");
      return 0;
    }

  kh = keydb_new (0);
  if (!kh)
    {
      log_error (_("failed to allocated keyDB handle\n"));
      rc = gpg_error (GPG_ERR_GENERAL);
      goto leave;
    }

  if (DBG_X509 && !listmode)
    gpgsm_dump_cert ("target", cert);

  subject_cert = cert;
  ksba_cert_ref (subject_cert);
  maxdepth = 50;

  for (;;)
    {
      int is_root;
      gpg_error_t istrusted_rc = -1;
      struct rootca_flags_s rootca_flags;

      xfree (issuer);
      xfree (subject);
      issuer = ksba_cert_get_issuer (subject_cert, 0);
      subject = ksba_cert_get_subject (subject_cert, 0);

      if (!issuer)
        {
          do_list (1, lm, fp,  _("no issuer found in certificate"));
          rc = gpg_error (GPG_ERR_BAD_CERT);
          goto leave;
        }

      /* Is this a self-issued certificate (i.e. the root certificate)? */
      is_root = (subject && !strcmp (issuer, subject));
      if (is_root)
        {
          /* Check early whether the certificate is listed as trusted.
             We used to do this only later but changed it to call the
             check right here so that we can access special flags
             associated with that specific root certificate.  */
          istrusted_rc = gpgsm_agent_istrusted (ctrl, subject_cert,
                                                &rootca_flags);
        }
      

      /* Check the validity period. */
      {
        ksba_isotime_t not_before, not_after;

        rc = ksba_cert_get_validity (subject_cert, 0, not_before);
        if (!rc)
          rc = ksba_cert_get_validity (subject_cert, 1, not_after);
        if (rc)
          {
            do_list (1, lm, fp, _("certificate with invalid validity: %s"),
                     gpg_strerror (rc));
            rc = gpg_error (GPG_ERR_BAD_CERT);
            goto leave;
          }

        if (*not_after)
          {
            if (!*exptime)
              gnupg_copy_time (exptime, not_after);
            else if (strcmp (not_after, exptime) < 0 )
              gnupg_copy_time (exptime, not_after);
          }

        if (*not_before && strcmp (current_time, not_before) < 0 )
          {
            do_list (1, lm, fp, _("certificate not yet valid"));
            if (!lm)
              {
                log_info ("(valid from ");
                gpgsm_dump_time (not_before);
                log_printf (")\n");
              }
            rc = gpg_error (GPG_ERR_CERT_TOO_YOUNG);
            goto leave;
          }            
        if (*not_after && strcmp (current_time, not_after) > 0 )
          {
            do_list (opt.ignore_expiration?0:1, lm, fp,
                     _("certificate has expired"));
            if (!lm)
              {
                log_info ("(expired at ");
                gpgsm_dump_time (not_after);
                log_printf (")\n");
              }
            if (opt.ignore_expiration)
                log_info ("WARNING: ignoring expiration\n");
            else
              any_expired = 1;
          }            
      }

      /* Assert that we understand all critical extensions. */
      rc = unknown_criticals (subject_cert, listmode, fp);
      if (rc)
        goto leave;

      /* Do a policy check. */
      if (!opt.no_policy_check)
        {
          rc = check_cert_policy (subject_cert, listmode, fp);
          if (gpg_err_code (rc) == GPG_ERR_NO_POLICY_MATCH)
            {
              any_no_policy_match = 1;
              rc = 1;
            }
          else if (rc)
            goto leave;
        }


      /* Is this a self-issued certificate? */
      if (is_root)
        { 
          if (!istrusted_rc)
            ; /* No need to check the certificate for a trusted one. */
          else if (gpgsm_check_cert_sig (subject_cert, subject_cert) )
            {
              /* We only check the signature if the certificate is not
                 trusted for better diagnostics. */
              do_list (1, lm, fp,
                       _("self-signed certificate has a BAD signature"));
              if (DBG_X509)
                {
                  gpgsm_dump_cert ("self-signing cert", subject_cert);
                }
              rc = gpg_error (depth? GPG_ERR_BAD_CERT_CHAIN
                                   : GPG_ERR_BAD_CERT);
              goto leave;
            }
          if (!rootca_flags.relax)
            {
              rc = allowed_ca (subject_cert, NULL, listmode, fp);
              if (rc)
                goto leave;
            }
              
          
          /* Set the flag for qualified signatures.  This flag is
             deduced from a list of root certificates allowed for
             qualified signatures. */
          if (is_qualified == -1)
            {
              gpg_error_t err;
              size_t buflen;
              char buf[1];
              
              if (!ksba_cert_get_user_data (cert, "is_qualified", 
                                            &buf, sizeof (buf),
                                            &buflen) && buflen)
                {
                  /* We already checked this for this certificate,
                     thus we simply take it from the user data. */
                  is_qualified = !!*buf;
                }    
              else
                {
                  /* Need to consult the list of root certificates for
                     qualified signatures. */
                  err = gpgsm_is_in_qualified_list (ctrl, subject_cert, NULL);
                  if (!err)
                    is_qualified = 1;
                  else if ( gpg_err_code (err) == GPG_ERR_NOT_FOUND)
                    is_qualified = 0;
                  else
                    log_error ("checking the list of qualified "
                               "root certificates failed: %s\n",
                               gpg_strerror (err));
                  if ( is_qualified != -1 )
                    {
                      /* Cache the result but don't care too much
                         about an error. */
                      buf[0] = !!is_qualified;
                      err = ksba_cert_set_user_data (subject_cert,
                                                     "is_qualified", buf, 1);
                      if (err)
                        log_error ("set_user_data(is_qualified) failed: %s\n",
                                   gpg_strerror (err)); 
                    }
                }
            }


          /* Act on the check for a trusted root certificates. */
          rc = istrusted_rc;
          if (!rc)
            ;
          else if (gpg_err_code (rc) == GPG_ERR_NOT_TRUSTED)
            {
              do_list (0, lm, fp, _("root certificate is not marked trusted"));
              /* If we already figured out that the certificate is
                 expired it does not make much sense to ask the user
                 whether we wants to trust the root certificate.  He
                 should do this only if the certificate under question
                 will then be usable.  We also check whether the agent
                 is at all enabled to allo marktrusted and don't call
                 it in this session again if it is not. */
              if ( !any_expired
                   && (!lm || !already_asked_marktrusted (subject_cert)))
                {
                  static int no_more_questions; /* during this session. */
                  int rc2;
                  char *fpr = gpgsm_get_fingerprint_string (subject_cert,
                                                            GCRY_MD_SHA1);
                  log_info (_("fingerprint=%s\n"), fpr? fpr : "?");
                  xfree (fpr);
                  if (no_more_questions)
                    rc2 = gpg_error (GPG_ERR_NOT_SUPPORTED);
                  else
                    rc2 = gpgsm_agent_marktrusted (ctrl, subject_cert);
                  if (!rc2)
                    {
                      log_info (_("root certificate has now"
                                  " been marked as trusted\n"));
                      rc = 0;
                    }
                  else if (!lm)
                    {
                      gpgsm_dump_cert ("issuer", subject_cert);
                      log_info ("after checking the fingerprint, you may want "
                                "to add it manually to the list of trusted "
                                "certificates.\n");
                    }

                  if (gpg_err_code (rc2) == GPG_ERR_NOT_SUPPORTED)
                    {
                      if (!no_more_questions)
                        log_info (_("interactive marking as trusted "
                                    "not enabled in gpg-agent\n"));
                      no_more_questions = 1;
                    }
                  else if (gpg_err_code (rc2) == GPG_ERR_CANCELED)
                    {
                      log_info (_("interactive marking as trusted "
                                  "disabled for this session\n"));
                      no_more_questions = 1;
                    }
                  else
                    set_already_asked_marktrusted (subject_cert);
                }
            }
          else 
            {
              log_error (_("checking the trust list failed: %s\n"),
                         gpg_strerror (rc));
            }
          
          if (rc)
            goto leave;

          /* Check for revocations etc. */
          if ((flags & 1))
            ;
          else if (opt.no_trusted_cert_crl_check || rootca_flags.relax)
            ; 
          else
            rc = is_cert_still_valid (ctrl, lm, fp,
                                      subject_cert, subject_cert,
                                      &any_revoked, &any_no_crl,
                                      &any_crl_too_old);
          if (rc)
            goto leave;

          break;  /* Okay: a self-signed certicate is an end-point. */
        }
      
      /* Take care that the chain does not get too long. */
      depth++;
      if (depth > maxdepth)
        {
          do_list (1, lm, fp, _("certificate chain too long\n"));
          rc = gpg_error (GPG_ERR_BAD_CERT_CHAIN);
          goto leave;
        }

      /* Find the next cert up the tree. */
      keydb_search_reset (kh);
      rc = find_up (kh, subject_cert, issuer, 0);
      if (rc)
        {
          if (rc == -1)
            {
              do_list (0, lm, fp, _("issuer certificate not found"));
              if (!lm)
                {
                  log_info ("issuer certificate: #/");
                  gpgsm_dump_string (issuer);
                  log_printf ("\n");
                }
            }
          else
            log_error ("failed to find issuer's certificate: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_MISSING_CERT);
          goto leave;
        }

      ksba_cert_release (issuer_cert); issuer_cert = NULL;
      rc = keydb_get_cert (kh, &issuer_cert);
      if (rc)
        {
          log_error ("keydb_get_cert() failed: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_GENERAL);
          goto leave;
        }

    try_another_cert:
      if (DBG_X509)
        {
          log_debug ("got issuer's certificate:\n");
          gpgsm_dump_cert ("issuer", issuer_cert);
        }

      rc = gpgsm_check_cert_sig (issuer_cert, subject_cert);
      if (rc)
        {
          do_list (0, lm, fp, _("certificate has a BAD signature"));
          if (DBG_X509)
            {
              gpgsm_dump_cert ("signing issuer", issuer_cert);
              gpgsm_dump_cert ("signed subject", subject_cert);
            }
          if (gpg_err_code (rc) == GPG_ERR_BAD_SIGNATURE)
            {
              /* We now try to find other issuer certificates which
                 might have been used.  This is required because some
                 CAs are reusing the issuer and subject DN for new
                 root certificates. */
              /* FIXME: Do this only if we don't have an
                 AKI.keyIdentifier */
              rc = find_up (kh, subject_cert, issuer, 1);
              if (!rc)
                {
                  ksba_cert_t tmp_cert;

                  rc = keydb_get_cert (kh, &tmp_cert);
                  if (rc || !compare_certs (issuer_cert, tmp_cert))
                    {
                      /* The find next did not work or returned an
                         identical certificate.  We better stop here
                         to avoid infinite checks. */
                      rc = gpg_error (GPG_ERR_BAD_SIGNATURE);
                      ksba_cert_release (tmp_cert);
                    }
                  else
                    {
                      do_list (0, lm, fp, _("found another possible matching "
                                            "CA certificate - trying again"));
                      ksba_cert_release (issuer_cert); 
                      issuer_cert = tmp_cert;
                      goto try_another_cert;
                    }
                }
            }

          /* We give a more descriptive error code than the one
             returned from the signature checking. */
          rc = gpg_error (GPG_ERR_BAD_CERT_CHAIN);
          goto leave;
        }

      is_root = 0;
      istrusted_rc = -1;

      /* Check that a CA is allowed to issue certificates. */
      {
        int chainlen;

        rc = allowed_ca (issuer_cert, &chainlen, listmode, fp);
        if (rc)
          {
            /* Not allowed.  Check whether this is a trusted root
               certificate and whether we allow special exceptions.
               We could carry the result of the test over to the
               regular root check at the top of the loop but for
               clarity we won't do that.  Given that the majority of
               certificates carry proper BasicContraints our way of
               overriding an error in the way is justified for
               performance reasons. */
            if (gpgsm_is_root_cert (issuer_cert))
              {
                is_root = 1;
                istrusted_rc = gpgsm_agent_istrusted (ctrl, issuer_cert,
                                                      &rootca_flags);
                if (!istrusted_rc && rootca_flags.relax)
                  {
                    /* Ignore the error due to the relax flag.  */
                    rc = 0;
                    chainlen = -1;
                  }
              }
          }
        if (rc)
          goto leave;
        if (chainlen >= 0 && (depth - 1) > chainlen)
          {
            do_list (1, lm, fp,
                     _("certificate chain longer than allowed by CA (%d)"),
                     chainlen);
            rc = gpg_error (GPG_ERR_BAD_CERT_CHAIN);
            goto leave;
          }
      }

      /* Is the certificate allowed to sign other certificates. */
      if (!listmode)
        {
          rc = gpgsm_cert_use_cert_p (issuer_cert);
          if (rc)
            {
              char numbuf[50];
              sprintf (numbuf, "%d", rc);
              gpgsm_status2 (ctrl, STATUS_ERROR, "certcert.issuer.keyusage",
                             numbuf, NULL);
              goto leave;
            }
        }

      /* Check for revocations etc.  Note that for a root certioficate
         this test is done a second time later. This should eventually
         be fixed. */
      if ((flags & 1))
        rc = 0;
      else if (is_root && (opt.no_trusted_cert_crl_check
                           || (!istrusted_rc && rootca_flags.relax)))
        ; 
      else
        rc = is_cert_still_valid (ctrl, lm, fp,
                                  subject_cert, issuer_cert,
                                  &any_revoked, &any_no_crl, &any_crl_too_old);
      if (rc)
        goto leave;


      if (opt.verbose && !listmode)
        log_info ("certificate is good\n");

      /* For the next round the current issuer becomes the new subject.  */
      keydb_search_reset (kh);
      ksba_cert_release (subject_cert);
      subject_cert = issuer_cert;
      issuer_cert = NULL;
    } /* End chain traversal. */

  if (!listmode)
    {
      if (opt.no_policy_check)
        log_info ("policies not checked due to %s option\n",
                  "--disable-policy-checks");
      if (opt.no_crl_check && !ctrl->use_ocsp)
        log_info ("CRLs not checked due to %s option\n",
                  "--disable-crl-checks");
    }

  if (!rc)
    { /* If we encountered an error somewhere during the checks, set
         the error code to the most critical one */
      if (any_revoked)
        rc = gpg_error (GPG_ERR_CERT_REVOKED);
      else if (any_expired)
        rc = gpg_error (GPG_ERR_CERT_EXPIRED);
      else if (any_no_crl)
        rc = gpg_error (GPG_ERR_NO_CRL_KNOWN);
      else if (any_crl_too_old)
        rc = gpg_error (GPG_ERR_CRL_TOO_OLD);
      else if (any_no_policy_match)
        rc = gpg_error (GPG_ERR_NO_POLICY_MATCH);
    }
  
 leave:
  if (is_qualified != -1)
    {
      /* We figured something about the qualified signature capability
         of the certificate under question.  Store the result as user
         data in the certificate object.  We do this even if the
         validation itself failed. */
      /* Fixme: We should set this flag for all certificates in the
         chain for optimizing reasons. */
      char buf[1];
      gpg_error_t err;

      buf[0] = !!is_qualified;
      err = ksba_cert_set_user_data (cert, "is_qualified", buf, 1);
      if (err)
        {
          log_error ("set_user_data(is_qualified) failed: %s\n",
                     gpg_strerror (err)); 
          if (!rc)
            rc = err;
        }
    }
  if (r_exptime)
    gnupg_copy_time (r_exptime, exptime);
  xfree (issuer);
  xfree (subject);
  keydb_release (kh); 
  ksba_cert_release (issuer_cert);
  ksba_cert_release (subject_cert);
  return rc;
}


/* Check that the given certificate is valid but DO NOT check any
   constraints.  We assume that the issuers certificate is already in
   the DB and that this one is valid; which it should be because it
   has been checked using this function. */
int
gpgsm_basic_cert_check (ksba_cert_t cert)
{
  int rc = 0;
  char *issuer = NULL;
  char *subject = NULL;
  KEYDB_HANDLE kh;
  ksba_cert_t issuer_cert = NULL;
  
  if (opt.no_chain_validation)
    {
      log_info ("WARNING: bypassing basic certificate checks\n");
      return 0;
    }

  kh = keydb_new (0);
  if (!kh)
    {
      log_error (_("failed to allocated keyDB handle\n"));
      rc = gpg_error (GPG_ERR_GENERAL);
      goto leave;
    }

  issuer = ksba_cert_get_issuer (cert, 0);
  subject = ksba_cert_get_subject (cert, 0);
  if (!issuer)
    {
      log_error ("no issuer found in certificate\n");
      rc = gpg_error (GPG_ERR_BAD_CERT);
      goto leave;
    }

  if (subject && !strcmp (issuer, subject))
    {
      rc = gpgsm_check_cert_sig (cert, cert);
      if (rc)
        {
          log_error ("self-signed certificate has a BAD signature: %s\n",
                     gpg_strerror (rc));
          if (DBG_X509)
            {
              gpgsm_dump_cert ("self-signing cert", cert);
            }
          rc = gpg_error (GPG_ERR_BAD_CERT);
          goto leave;
        }
    }
  else
    {
      /* Find the next cert up the tree. */
      keydb_search_reset (kh);
      rc = find_up (kh, cert, issuer, 0);
      if (rc)
        {
          if (rc == -1)
            {
              log_info ("issuer certificate (#/");
              gpgsm_dump_string (issuer);
              log_printf (") not found\n");
            }
          else
            log_error ("failed to find issuer's certificate: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_MISSING_CERT);
          goto leave;
        }
      
      ksba_cert_release (issuer_cert); issuer_cert = NULL;
      rc = keydb_get_cert (kh, &issuer_cert);
      if (rc)
        {
          log_error ("keydb_get_cert() failed: rc=%d\n", rc);
          rc = gpg_error (GPG_ERR_GENERAL);
          goto leave;
        }

      rc = gpgsm_check_cert_sig (issuer_cert, cert);
      if (rc)
        {
          log_error ("certificate has a BAD signature: %s\n",
                     gpg_strerror (rc));
          if (DBG_X509)
            {
              gpgsm_dump_cert ("signing issuer", issuer_cert);
              gpgsm_dump_cert ("signed subject", cert);
            }
          rc = gpg_error (GPG_ERR_BAD_CERT);
          goto leave;
        }
      if (opt.verbose)
        log_info ("certificate is good\n");
    }

 leave:
  xfree (issuer);
  xfree (subject);
  keydb_release (kh); 
  ksba_cert_release (issuer_cert);
  return rc;
}



/* Check whether the certificate CERT has been issued by the German
   authority for qualified signature.  They do not set the
   basicConstraints and thus we need this workaround.  It works by
   looking up the root certificate and checking whether that one is
   listed as a qualified certificate for Germany. 

   We also try to cache this data but as long as don't keep a
   reference to the certificate this won't be used.

   Returns: True if CERT is a RegTP issued CA cert (i.e. the root
   certificate itself or one of the CAs).  In that case CHAINLEN will
   receive the length of the chain which is either 0 or 1.
*/
static int
get_regtp_ca_info (ksba_cert_t cert, int *chainlen)
{
  gpg_error_t err;
  ksba_cert_t next;
  int rc = 0;
  int i, depth;
  char country[3];
  ksba_cert_t array[4];
  char buf[2];
  size_t buflen;
  int dummy_chainlen;

  if (!chainlen)
    chainlen = &dummy_chainlen;

  *chainlen = 0;
  err = ksba_cert_get_user_data (cert, "regtp_ca_chainlen", 
                                 &buf, sizeof (buf), &buflen);
  if (!err)
    {
      /* Got info. */
      if (buflen < 2 || !*buf)
        return 0; /* Nothing found. */
      *chainlen = buf[1];
      return 1; /* This is a regtp CA. */
    }
  else if (gpg_err_code (err) != GPG_ERR_NOT_FOUND)
    {
      log_error ("ksba_cert_get_user_data(%s) failed: %s\n",
                 "regtp_ca_chainlen", gpg_strerror (err));
      return 0; /* Nothing found.  */
    }

  /* Need to gather the info.  This requires to walk up the chain
     until we have found the root.  Because we are only interested in
     German Bundesnetzagentur (former RegTP) derived certificates 3
     levels are enough.  (The German signature law demands a 3 tier
     hierachy; thus there is only one CA between the EE and the Root
     CA.)  */
  memset (&array, 0, sizeof array);

  depth = 0;
  ksba_cert_ref (cert);
  array[depth++] = cert;
  ksba_cert_ref (cert);
  while (depth < DIM(array) && !(rc=gpgsm_walk_cert_chain (cert, &next)))
    {
      ksba_cert_release (cert);
      ksba_cert_ref (next);
      array[depth++] = next;
      cert = next;
    }
  ksba_cert_release (cert);
  if (rc != -1 || !depth || depth == DIM(array) )
    {
      /* We did not reached the root. */
      goto leave;
    }

  /* If this is a German signature law issued certificate, we store
     additional additional information. */
  if (!gpgsm_is_in_qualified_list (NULL, array[depth-1], country)
      && !strcmp (country, "de"))
    {
      /* Setting the pathlen for the root CA and the CA flag for the
         next one is all what we need to do. */
      err = ksba_cert_set_user_data (array[depth-1], "regtp_ca_chainlen",
                                     "\x01\x01", 2);
      if (!err && depth > 1)
        err = ksba_cert_set_user_data (array[depth-2], "regtp_ca_chainlen",
                                       "\x01\x00", 2);
      if (err)
        log_error ("ksba_set_user_data(%s) failed: %s\n",
                   "regtp_ca_chainlen", gpg_strerror (err)); 
      for (i=0; i < depth; i++)
        ksba_cert_release (array[i]);
      *chainlen = (depth>1? 0:1);
      return 1;
    }

 leave:
  /* Nothing special with this certificate. Mark the target
     certificate anyway to avoid duplicate lookups. */ 
  err = ksba_cert_set_user_data (cert, "regtp_ca_chainlen", "", 1);
  if (err)
    log_error ("ksba_set_user_data(%s) failed: %s\n",
               "regtp_ca_chainlen", gpg_strerror (err)); 
  for (i=0; i < depth; i++)
    ksba_cert_release (array[i]);
  return 0;
}