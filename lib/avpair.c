/*
 * $Id: avpair.c,v 1.7 2004/02/23 20:10:39 sobomax Exp $
 *
 * Copyright (C) 1995 Lars Fenneberg
 *
 * Copyright 1992 Livingston Enterprises, Inc.
 *
 * Copyright 1992,1993, 1994,1995 The Regents of the University of Michigan
 * and Merit Network, Inc. All Rights Reserved
 *
 * See the file COPYRIGHT for the respective terms and conditions.
 * If the file is missing contact me at lf@elemental.net
 * and I'll send you a copy.
 *
 */

#include <config.h>
#include <includes.h>
#include <radiusclient.h>

/*
 * Function: rc_avpair_add
 *
 * Purpose: add an attribute-value pair to the given list.
 *
 * Returns: pointer to added a/v pair upon success, NULL pointer upon failure.
 *
 * Remarks: Always appends the new pair to the end of the list.
 *
 */

VALUE_PAIR *rc_avpair_add (rc_handle *rh, VALUE_PAIR **list, int attrid, void *pval, int len, int vendorpec)
{
	VALUE_PAIR     *vp;

	vp = rc_avpair_new (rh, attrid, pval, len, vendorpec);

	if (vp != NULL)
	{
		rc_avpair_insert (list, NULL, vp);
	}

	return vp;

}

/*
 * Function: rc_avpair_assign
 *
 * Purpose: assign the given value to an attribute-value pair.
 *
 * Returns:  0 on success,
 *	    -1 on failure.
 *
 */

int rc_avpair_assign (VALUE_PAIR *vp, void *pval, int len)
{
	int	result = -1;

	switch (vp->type)
	{
		case PW_TYPE_STRING:

			if (((len == -1) && (strlen ((char *) pval)) > AUTH_STRING_LEN)
			    || (len > AUTH_STRING_LEN)) {
		        	rc_log(LOG_ERR, "rc_avpair_assign: bad attribute length");
		        	return result;
		    }

			if (len >= 0) {
				memcpy(vp->strvalue, (char *)pval, len);
				vp->strvalue[len] = '\0';
				vp->lvalue = len;
			} else {
		    	strncpy (vp->strvalue, (char *) pval, AUTH_STRING_LEN);
		    	vp->lvalue = strlen((char *) pval);
			}

			result = 0;
			break;

		case PW_TYPE_DATE:
		case PW_TYPE_INTEGER:
	        case PW_TYPE_IPADDR:

			vp->lvalue = * (UINT4 *) pval;

			result = 0;
			break;

		default:
			rc_log(LOG_ERR, "rc_avpair_assign: unknown attribute %d", vp->type);
	}
	return result;
}

/*
 * Function: rc_avpair_new
 *
 * Purpose: make a new attribute-value pair with given parameters.
 *
 * Returns: pointer to generated a/v pair when successful, NULL when failure.
 *
 */

VALUE_PAIR *rc_avpair_new (rc_handle *rh, int attrid, void *pval, int len, int vendorpec)
{
	VALUE_PAIR     *vp = NULL;
	DICT_ATTR      *pda;
	DICT_VENDOR    *vend;

	if ((pda = rc_dict_getattr (rh, attrid)) == NULL)
	{
		rc_log(LOG_ERR,"rc_avpair_new: unknown attribute %d", attrid);
		return NULL;
	}
	if (vendorpec != 0 && (vend = rc_dict_getvend(rh, vendorpec)) == NULL)
	{
		rc_log(LOG_ERR,"rc_avpair_new: unknown Vendor-Id %d", vendorpec);
		return NULL;
	}
	if ((vp = malloc (sizeof (VALUE_PAIR))) != NULL)
	{
		strncpy (vp->name, pda->name, sizeof (vp->name));
		vp->attribute = attrid | (vendorpec << 16);
		vp->next = NULL;
		vp->type = pda->type;
		if (rc_avpair_assign (vp, pval, len) == 0)
		{
			/* XXX: Fix up Digest-Attributes */
			switch (vp->attribute) {
			case PW_DIGEST_REALM:
			case PW_DIGEST_NONCE:
			case PW_DIGEST_METHOD:
			case PW_DIGEST_URI:
			case PW_DIGEST_QOP:
			case PW_DIGEST_ALGORITHM:
			case PW_DIGEST_BODY_DIGEST:
			case PW_DIGEST_CNONCE:
			case PW_DIGEST_NONCE_COUNT:
			case PW_DIGEST_USER_NAME:
				/* overlapping! */
				memmove(&vp->strvalue[2], &vp->strvalue[0], vp->lvalue);
				vp->strvalue[0] = vp->attribute - PW_DIGEST_REALM + 1;
				vp->lvalue += 2;
				vp->strvalue[1] = vp->lvalue;
				vp->attribute = PW_DIGEST_ATTRIBUTES;
			default:
				break;
			}
			return vp;
		}
		free (vp);
		vp = NULL;
	}
	else
	{
		rc_log(LOG_CRIT,"rc_avpair_new: out of memory");
	}

	return vp;
}

/*
 *
 * Function: rc_avpair_gen
 *
 * Purpose: takes attribute/value pairs from buffer and builds a
 *	    value_pair list using allocated memory.
 *
 * Returns: value_pair list or NULL on failure
 */

VALUE_PAIR *rc_avpair_gen (rc_handle *rh, AUTH_HDR *auth)
{
	int             length;
	int             x_len;
	int             attribute;
	int             attrlen;
	UINT4           lvalue;
	unsigned char         *x_ptr;
	unsigned char         *ptr;
	DICT_ATTR      *attr;
	DICT_VENDOR    *vend;
	VALUE_PAIR     *vp;
	VALUE_PAIR     *pair;
	unsigned char          hex[3];		/* For hex string conversion. */
	char            buffer[256];
	int		vendorlen;

	/*
	 * Extract attribute-value pairs
	 */
	ptr = auth->data;
	length = ntohs ((unsigned short) auth->length) - AUTH_HDR_LEN;
	vp = NULL;
	vend = NULL;
	vendorlen = 0;

	while (length > 0)
	{
		attribute = *ptr++;
		if (vendorlen > 0)
			attribute |= (vend->vendorpec << 16);
		attrlen = *ptr++;
		attrlen -= 2;
		length -= 2;

		if (attrlen < 0 || (attribute == PW_VENDOR_SPECIFIC && attrlen < 6))
		{
			rc_log(LOG_ERR, "rc_avpair_gen: received attribute with invalid length");
			break;
		}

		if (vendorlen <= 0 && attribute == PW_VENDOR_SPECIFIC) {
			int sublen, vendorpec;
			unsigned char *subptr;

			memcpy(&lvalue, ptr, 4);
			vendorpec = ntohl(lvalue);
			vend = rc_dict_getvend(rh, vendorpec);
			if (vend == NULL) {
				rc_log(LOG_WARNING, "rc_avpair_gen: received VSA attribute with unknown Vendor-Id %d",
					vendorpec);
				ptr += attrlen;
				length -= attrlen;
				vendorlen = 0;
				continue;
			}

			subptr = ptr + 4;
			sublen = attrlen - 4;
			while (sublen > 0) {
				if (subptr[1] < 2)	/* too short */
					break;
				if (subptr[1] > sublen)	/* too long */
					break;
				sublen -= subptr[1];	/* just right */
				subptr += subptr[1];
			}
			if (sublen != 0) {
				rc_log(LOG_WARNING, "rc_avpair_gen: received mailformed VSA attribute %d, ignoring",
					vendorpec);
				ptr += attrlen;
				length -= attrlen;
				vendorlen = 0;
				continue;
			}
			ptr += 4;
			vendorlen = attrlen - 4;
			attribute = *ptr++ | (vend->vendorpec << 16);
			attrlen   = *ptr++;
			attrlen -= 2;
			length -= 6;
		}

		if ((attr = rc_dict_getattr (rh, attribute)) == NULL)
		{
			*buffer= '\0';	/* Initial length. */
			for (x_ptr = ptr, x_len = attrlen ;
				x_len > 0 ;
				x_len--, x_ptr++)
			{
				sprintf (hex, "%2.2X", *x_ptr);
				strcat (buffer, hex);
			}
			if (VENDOR(attribute) == 0) {
				rc_log(LOG_WARNING, "rc_avpair_gen: received unknown attribute %d of length %d: 0x%s",
					attribute, attrlen, buffer);
			} else {
				rc_log(LOG_WARNING, "rc_avpair_gen: received unknown VSA attribute %d, vendor %d of length %d: 0x%s",
					attribute & 0xffff, VENDOR(attribute), attrlen, buffer);
			}
		}
		else
		{
			if ((pair = malloc (sizeof (VALUE_PAIR))) == NULL)
			{
				rc_log(LOG_CRIT, "rc_avpair_gen: out of memory");
				rc_avpair_free(vp);
				return NULL;
			}
			strcpy (pair->name, attr->name);
			pair->attribute = attr->value;
			pair->type = attr->type;
			pair->next = NULL;

			switch (attr->type)
			{

			    case PW_TYPE_STRING:
				memcpy (pair->strvalue, (char *) ptr, (size_t) attrlen);
				pair->strvalue[attrlen] = '\0';
				pair->lvalue = attrlen;
				rc_avpair_insert (&vp, NULL, pair);
				break;

			    case PW_TYPE_INTEGER:
			    case PW_TYPE_IPADDR:
				memcpy ((char *) &lvalue, (char *) ptr,
					sizeof (UINT4));
				pair->lvalue = ntohl (lvalue);
				rc_avpair_insert (&vp, NULL, pair);
				break;

			    default:
				rc_log(LOG_WARNING, "rc_avpair_gen: %s has unknown type", attr->name);
				free (pair);
				break;
			}
		}
		ptr += attrlen;
		length -= attrlen;
		if (vendorlen > 0) vendorlen -= (attrlen + 2);
	}
	return vp;
}

/*
 * Function: rc_avpair_get
 *
 * Purpose: Find the first attribute value-pair (which matches the given
 *          attribute) from the specified value-pair list.
 *
 * Returns: found value_pair
 *
 */

VALUE_PAIR *rc_avpair_get (VALUE_PAIR *vp, UINT4 attr, int vendorpec)
{
	for (; vp != NULL && (vp->attribute != attr || VENDOR(vp->attribute) != vendorpec); vp = vp->next)
	{
		continue;
	}
	return vp;
}

/*
 * Function: rc_avpair_insert
 *
 * Purpose: Given the address of an existing list "a" and a pointer
 *	    to an entry "p" in that list, add the value pair "b" to
 *	    the "a" list after the "p" entry.  If "p" is NULL, add
 *	    the value pair "b" to the end of "a".
 *
 */

void rc_avpair_insert (VALUE_PAIR **a, VALUE_PAIR *p, VALUE_PAIR *b)
{
	VALUE_PAIR     *this_node = NULL;
	VALUE_PAIR     *vp;

	if (b->next != NULL)
	{
		rc_log(LOG_CRIT, "rc_avpair_insert: value pair (0x%p) next ptr. (0x%p) not NULL", b, b->next);
		abort ();
	}

	if (*a == NULL)
	{
		*a = b;
		return;
	}

	vp = *a;

	if ( p == NULL) /* run to end of "a" list */
	{
		while (vp != NULL)
		{
			this_node = vp;
			vp = vp->next;
		}
	}
	else /* look for the "p" entry in the "a" list */
	{
		this_node = *a;
		while (this_node != NULL)
		{
			if (this_node == p)
			{
				break;
			}
			this_node = this_node->next;
		}
	}

	b->next = this_node->next;
	this_node->next = b;

	return;
}

/*
 * Function: rc_avpair_free
 *
 * Purpose: frees all value_pairs in the list
 *
 */

void rc_avpair_free (VALUE_PAIR *pair)
{
	VALUE_PAIR     *next;

	while (pair != NULL)
	{
		next = pair->next;
		free (pair);
		pair = next;
	}
}

/*
 * Function: rc_fieldcpy
 *
 * Purpose: Copy a data field from the buffer.  Advance the buffer
 *          past the data field.
 *
 */

static void rc_fieldcpy (char *string, char **uptr)
{
	char           *ptr;

	ptr = *uptr;
	if (*ptr == '"')
	{
		ptr++;
		while (*ptr != '"' && *ptr != '\0' && *ptr != '\n')
		{
			*string++ = *ptr++;
		}
		*string = '\0';
		if (*ptr == '"')
		{
			ptr++;
		}
		*uptr = ptr;
		return;
	}

	while (*ptr != ' ' && *ptr != '\t' && *ptr != '\0' && *ptr != '\n' &&
			*ptr != '=' && *ptr != ',')
	{
		*string++ = *ptr++;
	}
	*string = '\0';
	*uptr = ptr;
	return;
}


/*
 * Function: rc_avpair_parse
 *
 * Purpose: parses the buffer to extract the attribute-value pairs.
 *
 * Returns: 0 = successful parse of attribute-value pair,
 *	   -1 = syntax (or other) error detected.
 *
 */

#define PARSE_MODE_NAME		0
#define PARSE_MODE_EQUAL	1
#define PARSE_MODE_VALUE	2
#define PARSE_MODE_INVALID	3

int rc_avpair_parse (rc_handle *rh, char *buffer, VALUE_PAIR **first_pair)
{
	int             mode;
	char            attrstr[AUTH_ID_LEN];
	char            valstr[AUTH_ID_LEN];
	DICT_ATTR      *attr = NULL;
	DICT_VALUE     *dval;
	VALUE_PAIR     *pair;
	VALUE_PAIR     *link;
	struct tm      *tm;
	time_t          timeval;

	mode = PARSE_MODE_NAME;
	while (*buffer != '\n' && *buffer != '\0')
	{
		if (*buffer == ' ' || *buffer == '\t')
		{
			buffer++;
			continue;
		}

		switch (mode)
		{
		    case PARSE_MODE_NAME:		/* Attribute Name */
			rc_fieldcpy (attrstr, &buffer);
			if ((attr =
				rc_dict_findattr (rh, attrstr)) == NULL)
			{
				rc_log(LOG_ERR, "rc_avpair_parse: unknown attribute");
				if (*first_pair) {
					rc_avpair_free(*first_pair);
					*first_pair = NULL;
				}
				return -1;
			}
			mode = PARSE_MODE_EQUAL;
			break;

		    case PARSE_MODE_EQUAL:		/* Equal sign */
			if (*buffer == '=')
			{
				mode = PARSE_MODE_VALUE;
				buffer++;
			}
			else
			{
				rc_log(LOG_ERR, "rc_avpair_parse: missing or misplaced equal sign");
				if (*first_pair) {
					rc_avpair_free(*first_pair);
					*first_pair = NULL;
				}
				return -1;
			}
			break;

		    case PARSE_MODE_VALUE:		/* Value */
			rc_fieldcpy (valstr, &buffer);

			if ((pair = malloc (sizeof (VALUE_PAIR))) == NULL)
			{
				rc_log(LOG_CRIT, "rc_avpair_parse: out of memory");
				if (*first_pair) {
					rc_avpair_free(*first_pair);
					*first_pair = NULL;
				}
				return -1;
			}
			strcpy (pair->name, attr->name);
			pair->attribute = attr->value;
			pair->type = attr->type;

			switch (pair->type)
			{

			    case PW_TYPE_STRING:
				strcpy (pair->strvalue, valstr);
				pair->lvalue = strlen(valstr);
				break;

			    case PW_TYPE_INTEGER:
				if (isdigit (*valstr))
				{
					pair->lvalue = atoi (valstr);
				}
				else
				{
					if ((dval = rc_dict_findval (rh, valstr))
							== NULL)
					{
						rc_log(LOG_ERR, "rc_avpair_parse: unknown attribute value: %s", valstr);
						if (*first_pair) {
							rc_avpair_free(*first_pair);
							*first_pair = NULL;
						}
						free (pair);
						return -1;
					}
					else
					{
						pair->lvalue = dval->value;
					}
				}
				break;

			    case PW_TYPE_IPADDR:
                                pair->lvalue = rc_get_ipaddr(valstr);
				break;

			    case PW_TYPE_DATE:
				timeval = time (0);
				tm = localtime (&timeval);
				tm->tm_hour = 0;
				tm->tm_min = 0;
				tm->tm_sec = 0;
				rc_str2tm (valstr, tm);
#ifdef TIMELOCAL
				pair->lvalue = (UINT4) timelocal (tm);
#else	/* TIMELOCAL */
				pair->lvalue = (UINT4) mktime (tm);
#endif	/* TIMELOCAL */
				break;

			    default:
				rc_log(LOG_ERR, "rc_avpair_parse: unknown attribute type %d", pair->type);
				if (*first_pair) {
					rc_avpair_free(*first_pair);
					*first_pair = NULL;
				}
				free (pair);
				return -1;
			}
			pair->next = NULL;

			if (*first_pair == NULL)
			{
				*first_pair = pair;
			}
			else
			{
				link = *first_pair;
				while (link->next != NULL)
				{
					link = link->next;
				}
				link->next = pair;
			}

			mode = PARSE_MODE_NAME;
			break;

		    default:
			mode = PARSE_MODE_NAME;
			break;
		}
	}
	return 0;
}

/*
 * Function: rc_avpair_tostr
 *
 * Purpose: Translate an av_pair into two strings
 *
 * Returns: 0 on success, -1 on failure
 *
 */

int rc_avpair_tostr (rc_handle *rh, VALUE_PAIR *pair, char *name, int ln, char *value, int lv)
{
	DICT_VALUE     *dval;
	char            buffer[32];
	struct in_addr  inad;
	unsigned char         *ptr;

	*name = *value = '\0';

	if (!pair || pair->name[0] == '\0') {
		rc_log(LOG_ERR, "rc_avpair_tostr: pair is NULL or empty");
		return -1;
	}

	strncpy(name, pair->name, (size_t) ln);

	switch (pair->type)
	{
	    case PW_TYPE_STRING:
	    	lv--;
		ptr = (unsigned char *) pair->strvalue;
		while (*ptr != '\0')
		{
			if (!(isprint (*ptr)))
			{
				sprintf (buffer, "\\%03o", *ptr);
				strncat(value, buffer, (size_t) lv);
				lv -= 4;
				if (lv < 0) break;
			}
			else
			{
				strncat(value, ptr, 1);
				lv--;
				if (lv < 0) break;
			}
			ptr++;
		}
		break;

	    case PW_TYPE_INTEGER:
		dval = rc_dict_getval (rh, pair->lvalue, pair->name);
		if (dval != NULL)
		{
			strncpy(value, dval->name, (size_t) lv-1);
		}
		else
		{
			sprintf (buffer, "%ld", pair->lvalue);
			strncpy(value, buffer, (size_t) lv);
		}
		break;

	    case PW_TYPE_IPADDR:
		inad.s_addr = htonl(pair->lvalue);
		strncpy (value, inet_ntoa (inad), (size_t) lv-1);
		break;

	    case PW_TYPE_DATE:
		strftime (buffer, sizeof (buffer), "%m/%d/%y %H:%M:%S",
			  gmtime ((time_t *) & pair->lvalue));
		strncpy(value, buffer, lv-1);
		break;

	    default:
		rc_log(LOG_ERR, "rc_avpair_tostr: unknown attribute type %d", pair->type);
		return -1;
		break;
	}

	return 0;
}

/*
 * Function: rc_avpair_readin
 *
 * Purpose: get a sequence of attribute value pairs from the file input
 *	    and make them into a list of value_pairs
 *
 */

VALUE_PAIR *rc_avpair_readin(rc_handle *rh, FILE *input)
{
	VALUE_PAIR *vp = NULL;
	char buffer[1024], *q;

	while (fgets(buffer, sizeof(buffer), input) != NULL)
	{
		q = buffer;

		while(*q && isspace(*q)) q++;

		if ((*q == '\n') || (*q == '#') || (*q == '\0'))
			continue;

		if (rc_avpair_parse(rh, q, &vp) < 0) {
			rc_log(LOG_ERR, "rc_avpair_readin: malformed attribute: %s", buffer);
			rc_avpair_free(vp);
			return NULL;
		}
	}

	return vp;
}