/*
   Unix SMB/CIFS implementation.
   string substitution functions
   Copyright (C) Andrew Tridgell 1992-2000
   Copyright (C) Gerald Carter   2006

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "includes.h"
#include "substitute.h"
#include "system/passwd.h"
#include "secrets.h"
#include "auth.h"
#include "lib/util/string_wrappers.h"

/* Max DNS name is 253 + '\0' */
#define MACHINE_NAME_SIZE 254

static char local_machine[MACHINE_NAME_SIZE];
static char remote_machine[MACHINE_NAME_SIZE];

userdom_struct current_user_info;
static fstring remote_proto="UNKNOWN";

void set_remote_proto(const char *proto)
{
	fstrcpy(remote_proto, proto);
}

/**
 * Set the 'local' machine name
 * @param local_name the name we are being called
 * @param if this is the 'final' name for us, not be be changed again
 */
bool set_local_machine_name(const char *local_name, bool perm)
{
	static bool already_perm = false;
	char tmp[MACHINE_NAME_SIZE];

	if (already_perm) {
		return true;
	}

	strlcpy(tmp, local_name, sizeof(tmp));
	trim_char(tmp, ' ', ' ');

	alpha_strcpy(local_machine,
		     tmp,
		     SAFE_NETBIOS_CHARS,
		     sizeof(local_machine) - 1);
	if (!strlower_m(local_machine)) {
		return false;
	}

	already_perm = perm;

	return true;
}

const char *get_local_machine_name(void)
{
	if (local_machine[0] == '\0') {
		return lp_netbios_name();
	}

	return local_machine;
}

/**
 * Set the 'remote' machine name
 *
 * @param remote_name the name our client wants to be called by
 * @param if this is the 'final' name for them, not be be changed again
 */
bool set_remote_machine_name(const char *remote_name, bool perm)
{
	static bool already_perm = False;
	char tmp[MACHINE_NAME_SIZE];

	if (already_perm) {
		return true;
	}

	strlcpy(tmp, remote_name, sizeof(tmp));
	trim_char(tmp, ' ', ' ');

	alpha_strcpy(remote_machine,
		     tmp,
		     SAFE_NETBIOS_CHARS,
		     sizeof(remote_machine) - 1);
	if (!strlower_m(remote_machine)) {
		return false;
	}

	already_perm = perm;

	return true;
}

const char *get_remote_machine_name(void)
{
	return remote_machine;
}

static char sub_peeraddr[INET6_ADDRSTRLEN];
static const char *sub_peername = NULL;
static char sub_sockaddr[INET6_ADDRSTRLEN];

void sub_set_socket_ids(const char *peeraddr, const char *peername,
			const char *sockaddr)
{
	const char *addr = peeraddr;

	if (strnequal(addr, "::ffff:", 7)) {
		addr += 7;
	}
	strlcpy(sub_peeraddr, addr, sizeof(sub_peeraddr));

	if (sub_peername != NULL &&
			sub_peername != sub_peeraddr) {
		talloc_free(discard_const_p(char,sub_peername));
		sub_peername = NULL;
	}
	sub_peername = talloc_strdup(NULL, peername);
	if (sub_peername == NULL) {
		sub_peername = sub_peeraddr;
	}

	/*
	 * Shouldn't we do the ::ffff: cancellation here as well? The
	 * original code in talloc_sub_basic() did not do it, so I'm
	 * leaving it out here as well for compatibility.
	 */
	strlcpy(sub_sockaddr, sockaddr, sizeof(sub_sockaddr));
}

/*******************************************************************
 Setup the strings used by substitutions. Called per packet. Ensure
 %U name is set correctly also.

 smb_name must be sanitized by alpha_strcpy
********************************************************************/

void set_current_user_info(const char *smb_name, const char *unix_name,
			   const char *domain)
{
	static const void *last_smb_name;
	static const void *last_unix_name;
	static const void *last_domain;

	if (likely(last_smb_name == smb_name &&
	    last_unix_name == unix_name &&
	    last_domain == domain))
	{
		return;
	}

	fstrcpy(current_user_info.smb_name, smb_name);
	fstrcpy(current_user_info.unix_name, unix_name);
	fstrcpy(current_user_info.domain, domain);

	last_smb_name = smb_name;
	last_unix_name = unix_name;
	last_domain = domain;
}

/*******************************************************************
 Return the current active user name.
*******************************************************************/

const char *get_current_username(void)
{
	return current_user_info.smb_name;
}

const char *get_current_user_info_domain(void)
{
	return current_user_info.domain;
}

/*******************************************************************
 Given a pointer to a %$(NAME) in p and the whole string in str
 expand it as an environment variable.
 str must be a talloced string.
 Return a new allocated and expanded string.
 Based on code by Branko Cibej <branko.cibej@hermes.si>
 When this is called p points at the '%' character.
 May substitute multiple occurrencies of the same env var.
********************************************************************/

static char *realloc_expand_env_var(char *str, char *p)
{
	char *envname;
	char *envval;
	char *q, *r;
	int copylen;

	if (p[0] != '%' || p[1] != '$' || p[2] != '(') {
		return str;
	}

	/*
	 * Look for the terminating ')'.
	 */

	if ((q = strchr_m(p,')')) == NULL) {
		DEBUG(0,("expand_env_var: Unterminated environment variable [%s]\n", p));
		return str;
	}

	/*
	 * Extract the name from within the %$(NAME) string.
	 */

	r = p + 3;
	copylen = q - r;

	/* reserve space for use later add %$() chars */
	if ( (envname = talloc_array(talloc_tos(), char, copylen + 1 + 4)) == NULL ) {
		return NULL;
	}

	strncpy(envname,r,copylen);
	envname[copylen] = '\0';

	if ((envval = getenv(envname)) == NULL) {
		DEBUG(0,("expand_env_var: Environment variable [%s] not set\n", envname));
		TALLOC_FREE(envname);
		return str;
	}

	/*
	 * Copy the full %$(NAME) into envname so it
	 * can be replaced.
	 */

	copylen = q + 1 - p;
	strncpy(envname,p,copylen);
	envname[copylen] = '\0';
	r = realloc_string_sub(str, envname, envval);
	TALLOC_FREE(envname);

	return r;
}

/****************************************************************************
 Do some standard substitutions in a string.
 len is the length in bytes of the space allowed in string str. If zero means
 don't allow expansions.
****************************************************************************/

void standard_sub_basic(const char *smb_name, const char *domain_name,
			char *str, size_t len)
{
	char *s;

	if ( (s = talloc_sub_basic(talloc_tos(), smb_name, domain_name, str )) != NULL ) {
		strncpy( str, s, len );
	}

	TALLOC_FREE( s );
}

/*
 * Limit addresses to hexalpha charactes and underscore, safe for path
 * components for Windows clients.
 */
static void make_address_pathsafe(char *addr)
{
	while(addr && *addr) {
		if(!isxdigit(*addr)) {
			*addr = '_';
		}
		++addr;
	}
}

/****************************************************************************
 Do some standard substitutions in a string.
 This function will return a talloced string that has to be freed.
****************************************************************************/

char *talloc_sub_basic(TALLOC_CTX *mem_ctx,
			const char *smb_name,
			const char *domain_name,
			const char *str)
{
	char *b, *p, *s, *r, *a_string;
	fstring pidstr, vnnstr;
	const char *local_machine_name = get_local_machine_name();
	TALLOC_CTX *tmp_ctx = NULL;

	/* workaround to prevent a crash while looking at bug #687 */

	if (!str) {
		DEBUG(0,("talloc_sub_basic: NULL source string!  This should not happen\n"));
		return NULL;
	}

	a_string = talloc_strdup(mem_ctx, str);
	if (a_string == NULL) {
		DEBUG(0, ("talloc_sub_basic: Out of memory!\n"));
		return NULL;
	}

	tmp_ctx = talloc_stackframe();

	for (s = a_string; (p = strchr_m(s, '%')); s = a_string + (p - b)) {

		r = NULL;
		b = a_string;

		switch (*(p+1)) {
		case 'U' :
			r = strlower_talloc(tmp_ctx, smb_name);
			if (r == NULL) {
				goto error;
			}
			a_string = realloc_string_sub(a_string, "%U", r);
			break;
		case 'G' : {
			struct passwd *pass;
			bool is_domain_name = false;
			const char *sep = lp_winbind_separator();

			if (domain_name != NULL && domain_name[0] != '\0' &&
			    (lp_security() == SEC_ADS ||
			     lp_security() == SEC_DOMAIN)) {
				r = talloc_asprintf(tmp_ctx,
						    "%s%c%s",
						    domain_name,
						    *sep,
						    smb_name);
				is_domain_name = true;
			} else {
				r = talloc_strdup(tmp_ctx, smb_name);
			}
			if (r == NULL) {
				goto error;
			}

			pass = Get_Pwnam_alloc(tmp_ctx, r);
			if (pass != NULL) {
				char *group_name;

				group_name = gidtoname(pass->pw_gid);
				if (is_domain_name) {
					char *group_sep;
					group_sep = strchr_m(group_name, *sep);
					if (group_sep != NULL) {
						group_name = group_sep + 1;
					}
				}
				a_string = realloc_string_sub(a_string,
							      "%G",
							      group_name);
			}
			TALLOC_FREE(pass);
			break;
		}
		case 'D' :
			r = strupper_talloc(tmp_ctx, domain_name);
			if (r == NULL) {
				goto error;
			}
			a_string = realloc_string_sub(a_string, "%D", r);
			break;
		case 'I' : {
			a_string = realloc_string_sub(
				a_string, "%I",
				sub_peeraddr[0] ? sub_peeraddr : "0.0.0.0");
			break;
		}
		case 'J' : {
			r = talloc_strdup(tmp_ctx,
				sub_peeraddr[0] ? sub_peeraddr : "0.0.0.0");
			make_address_pathsafe(r);
			a_string = realloc_string_sub(a_string, "%J", r);
			break;
		}
		case 'i':
			a_string = realloc_string_sub(
				a_string, "%i",
				sub_sockaddr[0] ? sub_sockaddr : "0.0.0.0");
			break;
		case 'j' : {
			r = talloc_strdup(tmp_ctx,
				sub_sockaddr[0] ? sub_sockaddr : "0.0.0.0");
			make_address_pathsafe(r);
			a_string = realloc_string_sub(a_string, "%j", r);
			break;
		}
		case 'L' :
			if ( strncasecmp_m(p, "%LOGONSERVER%", strlen("%LOGONSERVER%")) == 0 ) {
				break;
			}
			if (local_machine_name && *local_machine_name) {
				a_string = realloc_string_sub(a_string, "%L", local_machine_name);
			} else {
				a_string = realloc_string_sub(a_string, "%L", lp_netbios_name());
			}
			break;
		case 'N' :
			a_string = realloc_string_sub(a_string,
						      "%N",
						      lp_netbios_name());
			break;
		case 'M' :
			a_string = realloc_string_sub(a_string, "%M",
						      sub_peername ? sub_peername : "");
			break;
		case 'R' :
			a_string = realloc_string_sub(a_string, "%R", remote_proto);
			break;
		case 'T' :
			a_string = realloc_string_sub(a_string, "%T", current_timestring(tmp_ctx, False));
			break;
		case 't' :
			a_string = realloc_string_sub(a_string, "%t",
						      current_minimal_timestring(tmp_ctx, False));
			break;
		case 'a' :
			a_string = realloc_string_sub(a_string, "%a",
					get_remote_arch_str());
			break;
		case 'd' :
			slprintf(pidstr,sizeof(pidstr)-1, "%d",(int)getpid());
			a_string = realloc_string_sub(a_string, "%d", pidstr);
			break;
		case 'h' :
			a_string = realloc_string_sub(a_string, "%h", myhostname());
			break;
		case 'm' :
			a_string = realloc_string_sub(a_string, "%m",
						      remote_machine);
			break;
		case 'v' :
			a_string = realloc_string_sub(a_string, "%v", samba_version_string());
			break;
		case 'w' :
			a_string = realloc_string_sub(a_string, "%w", lp_winbind_separator());
			break;
		case '$' :
			a_string = realloc_expand_env_var(a_string, p); /* Expand environment variables */
			break;
		case 'V' :
			slprintf(vnnstr,sizeof(vnnstr)-1, "%u", get_my_vnn());
			a_string = realloc_string_sub(a_string, "%V", vnnstr);
			break;
		default:
			break;
		}

		p++;
		TALLOC_FREE(r);

		if (a_string == NULL) {
			goto done;
		}
	}

	goto done;

error:
	TALLOC_FREE(a_string);

done:
	TALLOC_FREE(tmp_ctx);
	return a_string;
}

/****************************************************************************
 Do some specific substitutions in a string.
 This function will return an allocated string that have to be freed.
****************************************************************************/

char *talloc_sub_specified(TALLOC_CTX *mem_ctx,
			const char *input_string,
			const char *username,
			const char *grpname,
			const char *domain,
			uid_t uid,
			gid_t gid)
{
	char *a_string;
	char *ret_string = NULL;
	char *b, *p, *s;
	TALLOC_CTX *tmp_ctx;

	if (!(tmp_ctx = talloc_new(mem_ctx))) {
		DEBUG(0, ("talloc_new failed\n"));
		return NULL;
	}

	a_string = talloc_strdup(tmp_ctx, input_string);
	if (a_string == NULL) {
		DEBUG(0, ("talloc_sub_specified: Out of memory!\n"));
		goto done;
	}

	for (s = a_string; (p = strchr_m(s, '%')); s = a_string + (p - b)) {

		b = a_string;

		switch (*(p+1)) {
		case 'U' :
			a_string = talloc_string_sub(
				tmp_ctx, a_string, "%U", username);
			break;
		case 'u' :
			a_string = talloc_string_sub(
				tmp_ctx, a_string, "%u", username);
			break;
		case 'G' :
			if (gid != -1) {
				const char *name;

				if (grpname != NULL) {
					name = grpname;
				} else {
					name = gidtoname(gid);
				}

				a_string = talloc_string_sub(tmp_ctx,
							     a_string,
							     "%G",
							     name);
			} else {
				a_string = talloc_string_sub(
					tmp_ctx, a_string,
					"%G", "NO_GROUP");
			}
			break;
		case 'g' :
			if (gid != -1) {
				const char *name;

				if (grpname != NULL) {
					name = grpname;
				} else {
					name = gidtoname(gid);
				}

				a_string = talloc_string_sub(tmp_ctx,
							     a_string,
							     "%g",
							     name);
			} else {
				a_string = talloc_string_sub(
					tmp_ctx, a_string, "%g", "NO_GROUP");
			}
			break;
		case 'D' :
			a_string = talloc_string_sub(tmp_ctx, a_string,
						     "%D", domain);
			break;
		case 'N' :
			a_string = talloc_string_sub(tmp_ctx, a_string,
						     "%N", lp_netbios_name());
			break;
		default:
			break;
		}

		p++;
		if (a_string == NULL) {
			goto done;
		}
	}

	/* Watch out, using "mem_ctx" here, so all intermediate stuff goes
	 * away with the TALLOC_FREE(tmp_ctx) further down. */

	ret_string = talloc_sub_basic(mem_ctx, username, domain, a_string);

 done:
	TALLOC_FREE(tmp_ctx);
	return ret_string;
}

/****************************************************************************
****************************************************************************/

char *talloc_sub_advanced(TALLOC_CTX *ctx,
			const char *servicename,
			const char *user,
			const char *connectpath,
			gid_t gid,
			const char *str)
{
	char *a_string;
	char *b, *p, *s;

	a_string = talloc_strdup(talloc_tos(), str);
	if (a_string == NULL) {
		DEBUG(0, ("talloc_sub_advanced_only: Out of memory!\n"));
		return NULL;
	}

	for (s = a_string; (p = strchr_m(s, '%')); s = a_string + (p - b)) {

		b = a_string;

		switch (*(p+1)) {
		case 'N':
			a_string = realloc_string_sub(a_string,
						      "%N",
						      lp_netbios_name());
			break;
		case 'H': {
			char *h;
			if ((h = get_user_home_dir(talloc_tos(), user)))
				a_string = realloc_string_sub(a_string, "%H", h);
			TALLOC_FREE(h);
			break;
		}
		case 'P':
			a_string = realloc_string_sub(a_string, "%P", connectpath);
			break;
		case 'S':
			a_string = realloc_string_sub(a_string, "%S", servicename);
			break;
		case 'g':
			a_string = realloc_string_sub(a_string, "%g", gidtoname(gid));
			break;
		case 'u':
			a_string = realloc_string_sub(a_string, "%u", user);
			break;
		default:
			break;
		}

		p++;
		if (a_string == NULL) {
			return NULL;
		}
	}

	return a_string;
}

char *talloc_sub_full(TALLOC_CTX *ctx,
			const char *servicename,
			const char *user,
			const char *connectpath,
			gid_t gid,
			const char *smb_name,
			const char *domain_name,
			const char *str)
{
	char *a_string, *ret_string;

	a_string = talloc_sub_advanced(ctx, servicename, user, connectpath,
				       gid, str);
	if (a_string == NULL) {
		return NULL;
	}

	ret_string = talloc_sub_basic(ctx, smb_name, domain_name, a_string);
	TALLOC_FREE(a_string);
	return ret_string;
}
