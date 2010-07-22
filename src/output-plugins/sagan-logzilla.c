/*
** Copyright (C) 2009-2010 Softwink, Inc. 
** Copyright (C) 2009-2010 Champ Clark III <champ@softwink.com>
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License Version 2 as
** published by the Free Software Foundation.  You may not use, modify or
** distribute this program under any other version of the GNU General
** Public License.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
*/

/* sagan-logzilla.c
 *
 * Logs to a Logzilla SQL database.  
 * See http://code.google.com/p/php-syslog-ng/
 *
 */


#ifdef HAVE_CONFIG_H
#include "config.h"             /* From autoconf */
#endif

#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

#include <stdio.h>
#include <string.h>

#include "sagan.h"
#include "version.h"
#include <pthread.h>


#ifdef HAVE_LIBMYSQLCLIENT_R
#include <mysql/mysql.h>
MYSQL    *connection, *mysql_logzilla;
#endif

#ifdef HAVE_LIBPQ
#include <libpq-fe.h>
PGconn   *psql_logzilla;
PGresult *result;
char pgconnect[2048];
#endif

int logzilla_log;
int logzilla_dbtype;
char logzilla_user[MAXUSER];
char logzilla_password[MAXPASS];
char logzilla_dbname[MAXDBNAME];
char logzilla_dbhost[MAXHOST];

int  threadlogzillac;

pthread_mutex_t logzilla_db_mutex;

int logzilla_db_connect( void ) {

char pgconnect[2048];

char *dbh=NULL;
char *dbu=NULL;
char *dbp=NULL;
char *dbn=NULL;

dbu = logzilla_user;
dbh = logzilla_dbhost;
dbp = logzilla_password;
dbn = logzilla_dbname;

/********************/
/* MySQL connection */
/********************/

#ifdef HAVE_LIBMYSQLCLIENT_R
if ( logzilla_dbtype == 1 ) {

mysql_thread_init();
mysql_logzilla = mysql_init(NULL);

if ( mysql_logzilla == NULL ) {
   removelockfile();
   sagan_log(1, "[%s, line %d] Error initializing MySQL", __FILE__, __LINE__ );
   }


my_bool reconnect = 1;
mysql_options(mysql_logzilla,MYSQL_READ_DEFAULT_GROUP,logzilla_dbname);

/* Re-connect to the database if the connection is lost */

mysql_options(mysql_logzilla,MYSQL_OPT_RECONNECT, &reconnect);

if (!mysql_real_connect(mysql_logzilla, dbh, dbu, dbp, dbn, MYSQL_PORT, NULL, 0)) {
     sagan_log(1, "[%s, line %d] MySQL Error %u: \"%s\"", __FILE__,  __LINE__, mysql_errno(mysql_logzilla), mysql_error(mysql_logzilla));
     }

}
#endif
/*************************/
/* PostgreSQL connection */
/*************************/

#ifdef HAVE_LIBPQ
if ( logzilla_dbtype == 2 ) {

//isthreadsafe = PQisthreadsafe();      // check

snprintf(pgconnect, sizeof(pgconnect), "hostaddr = '%s' port = '%d' dbname = '%s' user = '%s' password = '%s' connect_timeout = '30'", dbh, 5432 , dbn, dbu, dbp);

psql_logzilla = PQconnectdb(pgconnect);

if (!psql_logzilla) {
   removelockfile();
   sagan_log(1, "[%s, line %d] PostgreSQL: PQconnect Error", __FILE__, __LINE__);
   }

if (PQstatus(psql_logzilla) != CONNECTION_OK) {
   removelockfile();
   sagan_log(1, "[%s, line %d] PostgreSQL status not OK", __FILE__, __LINE__);
   }

}
#endif

return(0);
}  /* End of logzilla_connect */


/****************************************************************************
 * Query Database | iorq == 0 (SELECT) iorq == 1 (INSERT)                   *
 * For SELECT,  we typically only want one value back (row[0]) so return it *
 * For INSERT,  we don't need or get any results back                       *
 ****************************************************************************/

char *logzilla_db_query ( int dbtype,  char *sql ) {

pthread_mutex_lock( &logzilla_db_mutex );

char sqltmp[MAXSQL];    /* Make this a MAXSQL or something */
char *re=NULL;          /* "return" point for row */

strlcpy(sqltmp, sql, sizeof(sqltmp));

#ifdef HAVE_LIBMYSQLCLIENT_R
if ( logzilla_dbtype == 1 ) {

MYSQL_RES *logzilla_res;
MYSQL_ROW logzilla_row;

if ( mysql_real_query(mysql_logzilla, sqltmp,  strlen(sqltmp))) {
   removelockfile();
   sagan_log(1, "[%s, line %d] MySQL Error [%u:] \"%s\"\nOffending SQL statement: %s", __FILE__, __LINE__, mysql_errno(mysql_logzilla), mysql_error(mysql_logzilla), sqltmp);
   }

logzilla_res = mysql_use_result(mysql_logzilla);

if ( logzilla_res != NULL ) {
   while(logzilla_row = mysql_fetch_row(logzilla_res)) {
   snprintf(sqltmp, sizeof(sqltmp), "%s", logzilla_row[0]);
   re=sqltmp;
   }
 }

mysql_free_result(logzilla_res);
pthread_mutex_unlock( &logzilla_db_mutex );
return(re);
}
#else
removelockfile();
sagan_log(1, "Sagan was not compiled with MySQL support.  Aborting!");
#endif

#ifdef HAVE_LIBPQ
if ( logzilla_dbtype == 2 ) {

if (( result = PQexec(psql_logzilla, sql )) == NULL ) {
   removelockfile();
   sagan_log(1, "[%s, line %d] PostgreSQL Error: %s", __FILE__, __LINE__, PQerrorMessage( psql_logzilla ));
   }

if ( PQntuples(result) != 0 ) {
    re = PQgetvalue(result,0,0);
    }

PQclear(result);
pthread_mutex_unlock( &logzilla_db_mutex);
return(re);

}
#else
removelockfile();
sagan_log(1, "[%s, line %d] Sagan was not compiled with PostgreSQL support.  Aborting!", __FILE__, __LINE__);
#endif

return(0);
}


void *sagan_logzilla_thread ( void *logzillathreadargs ) { 

struct logzilla_thread_args * eargs = (struct logzilla_thread_args *) logzillathreadargs;

char sqltmp[MAXSQL];
char *sql=NULL;

char escprg[MAXPROGRAM];
char escmsg[MAX_SYSLOGMSG];

snprintf(escprg, sizeof(escprg), "%s", sql_escape(eargs->program, 1));
snprintf(escmsg, sizeof(escmsg), "%s", sql_escape(eargs->msg, 1));

snprintf(sqltmp, sizeof(sqltmp), "INSERT INTO logs (host, facility, priority, level, tag, program, msg, fo, lo) VALUES ('%s', '%s', '%s', '%s', '%s', %s, %s, '%s %s', '%s %s')", eargs->host, eargs->facility, eargs->priority, eargs->level, eargs->tag, escprg , escmsg, eargs->date, eargs->time, eargs->date, eargs->time  );


sql=sqltmp;
logzilla_db_query(logzilla_dbtype, sql);

threadlogzillac--;

return(0);
}

#endif