/*
 * Copyright (c) 1991, 1992, 1993 Brad Eacker,
 *              (Music, Intuition, Software, and Computers)
 * All Rights Reserved
 */

#include <stdio.h>
#include <fcntl.h>
#ifdef HAVE_SYS_FILE_H
# include <sys/file.h>
#endif

#include "php.h"
#include "ext/standard/flock_compat.h" 
#include "dbf.h"

void free_dbf_head(dbhead_t *dbh);
int get_dbf_field(dbhead_t *dbh, dbfield_t *dbf);

/*
 * get the header info from the file
 *	basic header info & field descriptions
 */
dbhead_t *get_dbf_head(int fd)
{
	dbhead_t *dbh;
	struct dbf_dhead  dbhead;
	dbfield_t *dbf, *cur_f, *tdbf;
	int ret, nfields, offset, gf_retval;
	int nullable_bit = 0;

	dbh = (dbhead_t *)ecalloc(1, sizeof(dbhead_t));
	if (lseek(fd, 0, 0) < 0) {
		efree(dbh);
		return NULL;
	}
	if ((ret = read(fd, &dbhead, sizeof(dbhead)))  != sizeof(dbhead)) {
		efree(dbh);
		return NULL;
	}

	/* build in core info */
	dbh->db_fd = fd;
	dbh->db_dbt = dbhead.dbh_dbt;
	dbh->db_records = get_long(dbhead.dbh_records);
	dbh->db_hlen = get_short(dbhead.dbh_hlen);
	dbh->db_rlen = get_short(dbhead.dbh_rlen);

	db_set_date(dbh->db_date, dbhead.dbh_date[DBH_DATE_YEAR] + 1900,
		dbhead.dbh_date[DBH_DATE_MONTH],
		dbhead.dbh_date[DBH_DATE_DAY]);

	/* malloc enough memory for the maximum number of fields: */
	tdbf = (dbfield_t *)ecalloc(DBH_MAX_FIELDS, sizeof(dbfield_t));
	
	offset = 1;
	nfields = 0;
	gf_retval = 0;
	for (cur_f = tdbf; gf_retval < 2 && nfields < DBH_MAX_FIELDS; cur_f++) {
		gf_retval = get_dbf_field(dbh, cur_f);

		if (gf_retval < 0) {
			goto fail;
		}
		if (gf_retval != 2 ) {
			cur_f->db_foffset = offset;
			offset += cur_f->db_flen;
			if (cur_f->db_fnullable) {
				cur_f->db_fnullable = nullable_bit++;
			} else {
				cur_f->db_fnullable = -1;
			}
			nfields++;
		}
	}

	for (cur_f = tdbf; cur_f < &tdbf[nfields - 1]; cur_f++) {
		if (cur_f->db_type == '0') {
			php_error_docref(NULL, E_WARNING, "unexpected field type '0'");
			goto fail;
		}
	}
	if (cur_f->db_type == '0') {
		if (!strcmp(cur_f->db_fname, "_NullFlags")) {
			dbh->db_nnullable = nullable_bit;
		} else {
			php_error_docref(NULL, E_WARNING, "unexpected field type '0'");
			goto fail;
		}
	} else {
		dbh->db_nnullable = 0;
	}

	dbh->db_nfields = nfields;

	/* malloc the right amount of space for records, copy and destroy old */
	dbf = (dbfield_t *)emalloc(sizeof(dbfield_t)*nfields);
	memcpy(dbf, tdbf, sizeof(dbfield_t)*nfields);
	efree(tdbf);

	dbh->db_fields = dbf;

	return dbh;
fail:
	for (cur_f = tdbf; cur_f < &tdbf[nfields]; cur_f++) {
		if (cur_f->db_format) {
			efree(cur_f->db_format);
		}
	}
	free_dbf_head(dbh);
	efree(tdbf);
	return NULL;
}

/*
 * free up the header info built above
 */
void free_dbf_head(dbhead_t *dbh)
{
	dbfield_t *dbf, *cur_f;
	int nfields;

	dbf = dbh->db_fields;
	nfields = dbh->db_nfields;
	for (cur_f = dbf; cur_f < &dbf[nfields]; cur_f++) {
		if (cur_f->db_format) {
			efree(cur_f->db_format);
		}
	}
	
	efree(dbf);
	efree(dbh);
}

/*
 * put out the header info
 * returns -1 on failure, != -1 on success
 */
int put_dbf_head(dbhead_t *dbh)
{
	int fd = dbh->db_fd;
	struct dbf_dhead  dbhead;
	int	ret;

	memset (&dbhead, 0, sizeof(dbhead));

	/* build on disk info */
	dbhead.dbh_dbt = dbh->db_dbt;
	put_long(dbhead.dbh_records, dbh->db_records);
	put_short(dbhead.dbh_hlen, dbh->db_hlen);
	put_short(dbhead.dbh_rlen, dbh->db_rlen);

	/* put the date spec'd into the on disk header */
	dbhead.dbh_date[DBH_DATE_YEAR] =(char)(db_date_year(dbh->db_date) -
						1900);
	dbhead.dbh_date[DBH_DATE_MONTH]=(char)(db_date_month(dbh->db_date));
	dbhead.dbh_date[DBH_DATE_DAY] =(char)(db_date_day(dbh->db_date));

	if (lseek(fd, 0, 0) < 0)
		return -1;
	if ((ret = write(fd, &dbhead, sizeof(dbhead))) != sizeof(dbhead))
		return -1;
	return ret;
}

/*
 * get a field off the disk from the current file offset
 * returns 0 on success, 2 on field terminator and -1 on failure
 */
int get_dbf_field(dbhead_t *dbh, dbfield_t *dbf)
{
	struct dbf_dfield	dbfield;
	int ret;

	if ((ret = read(dbh->db_fd, &dbfield, sizeof(dbfield))) <= 0) {
		return -1;
	}

	/* Check for the '0Dh' field terminator , if found return '2'
	   which will tell the loop we are at the end of fields */
	if (dbfield.dbf_name[0]==0x0d) {
		return 2;
	}

	if (ret != sizeof(dbfield)) {
		return -1;
	}

	/* build the field name */
	copy_crimp(dbf->db_fname, dbfield.dbf_name, DBF_NAMELEN);

	dbf->db_type = dbfield.dbf_type;
	switch (dbf->db_type) {
	    case 'N':
	    case 'F':
			dbf->db_flen = dbfield.dbf_flen[0];
			dbf->db_fdc = dbfield.dbf_flen[1];
			break;
	    case 'L':
			dbf->db_flen = 1;
			break;
	    case 'D':
	    case 'T':
			dbf->db_flen = 8;
			break;
	    default:
	    	dbf->db_flen = get_short(dbfield.dbf_flen);
			break;
	}

	if ((dbf->db_format = get_dbf_f_fmt(dbf)) == NULL) {
		php_error_docref(NULL, E_WARNING, "unknown field type '%c'", dbf->db_type);
		return -1;
	}

	if (dbh->db_dbt == DBH_TYPE_FOXPRO) {
		dbf->db_fnullable = dbfield.dbf_flags & 0x2;
	}

	return 0;
}

/*
 * put a field out on the disk at the current file offset
 * returns 1 on success, != 1 on failure
 */
int put_dbf_field(dbhead_t *dbh, dbfield_t *dbf)
{
	struct dbf_dfield	dbfield;
	int			ret;

	memset (&dbfield, 0, sizeof(dbfield));

	strlcpy(dbfield.dbf_name, dbf->db_fname, DBF_NAMELEN);

	dbfield.dbf_type = dbf->db_type;
	switch (dbf->db_type) {
	    case 'F':
	    case 'N':		
		dbfield.dbf_flen[0] = dbf->db_flen;
		dbfield.dbf_flen[1] = dbf->db_fdc;
		break;
	    case 'D':
		dbf->db_flen = 8;
		put_short(dbfield.dbf_flen, dbf->db_flen);
		break;
	    case 'L':
		dbf->db_flen = 1;
		put_short(dbfield.dbf_flen, dbf->db_flen);
		break;
	    default:
	    	put_short(dbfield.dbf_flen, dbf->db_flen);
	}

	if (dbh->db_dbt == DBH_TYPE_FOXPRO) {
		if (dbf->db_fnullable >= 0) {
			dbfield.dbf_flags = 0x2;
		}
		if (dbf->db_type == '0') {
			dbfield.dbf_flags = 0x5;
		}
	}

	/* now write it out to disk */
	if ((ret = write(dbh->db_fd, &dbfield, sizeof(dbfield))) != sizeof(dbfield)) {
		return ret;
	}
	return 1;
}

/*
 * put out all the info at the top of the file...
 * returns 1 on success, != 1 on failure
 */
static char end_stuff[2] = {0x0d, 0};

int put_dbf_info(dbhead_t *dbh)
{
	dbfield_t	*dbf;
	char		*cp;
	int		fcnt;
	char buf[263];

	if ((cp = db_cur_date(NULL))) {
		strlcpy(dbh->db_date, cp, 9);
		efree(cp);
	}
	if (put_dbf_head(dbh) < 0) {
		goto fail;
	}
	dbf = dbh->db_fields;
	for (fcnt = dbh->db_nfields; fcnt > 0; fcnt--, dbf++) {
		if (put_dbf_field(dbh, dbf) != 1) {
			goto fail;
		}
	}
	if (write(dbh->db_fd, end_stuff, 1) != 1) {
		goto fail;
	}
	if (dbh->db_dbt == DBH_TYPE_FOXPRO) {
		memset(&buf, 0, sizeof(buf));
		if (write(dbh->db_fd, &buf, sizeof(buf)) != sizeof(buf)) {
			goto fail;
		}
	}
	return 1;
fail:
	php_error_docref(NULL, E_WARNING, "unable to write dbf header");
	return -1;
}

char *get_dbf_f_fmt(dbfield_t *dbf)
{
	char format[100];

	/* build the field format for printf */
	switch (dbf->db_type) {
	   case 'C':
		snprintf(format, sizeof(format), "%%-%ds", dbf->db_flen);
		break;
	   case 'N':
	   case 'L':
	   case 'D':
	   case 'F':
		snprintf(format, sizeof(format), "%%%ds", dbf->db_flen);
		break;
	   case 'M':
		strlcpy(format, "%s", sizeof(format));
		break;
	   case 'T':
	   case '0':
		format[0] = '\0';
		break;
	   default:
		return NULL;
	}
	return (char *)estrdup(format);
}

dbhead_t *dbf_open(char *dp, int o_flags)
{
	int fd;
	char *cp;
	dbhead_t *dbh;

	cp = dp;
	if ((fd = VCWD_OPEN(cp, o_flags|O_BINARY)) < 0) {
		return NULL;
	}

	if ((dbh = get_dbf_head(fd)) ==	NULL) {
		close(fd);
		return NULL;
	}

	dbh->db_cur_rec = 0;
	return dbh;
}
 
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: sw=4 ts=4 fdm=marker
 * vim<600: sw=4 ts=4
 */
