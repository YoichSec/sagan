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

/* sagan.c 
 *
 * This is the main "thread" and engine that looks for events & patterns 
 * based on 'snort like' rule sets.   Some ideas and code where ripped
 * from Barnyard. 
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"             /* From autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include <pcre.h>
#include <limits.h>

#include "sagan.h"
#include "version.h"


#define OVECCOUNT 30


struct rule_struct *rulestruct;
struct class_struct *classstruct;

int sagan_find_ip=0;
int daemonize=0;
int programmode=0;
int dochroot=0;

unsigned long long sigcid;		/* Used for CID passing when signal's are caught */

char sagan_hostname[50]="localhost";
char sagan_interface[50]="syslog";
char sagan_filter[50]="none";
int  sagan_detail=1;
int  sensor_id;

char saganconf[MAXPATH]=CONFIG_FILE_PATH;
char *runas=RUNAS;

int  threadextc=0;
int  threadmaxextc=0;
int  dropped=0;
int  classcount=0;
int  threadid =0;

char sagan_host[MAXHOST];
char sagan_port[6];
char sagan_extern[MAXPATH];
char sagan_path[MAXPATH];

int  sagan_exttype=0;

unsigned long long threshold_total=0;
unsigned long long int sagantotal=0;
unsigned long long int saganfound=0;
unsigned long long int sagandrop=0;

char *defaultpri="0";                           /* Default priority */

int debug=0;
int fpri=0;
int devdebug=0;

int rulecount=0;
int ruletotal=0;

char fifo[MAX_SYSLOGMSG]="";
int  fifoi=0;

pthread_mutex_t ext_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t general_mutex = PTHREAD_MUTEX_INITIALIZER;

int max_ext_threads;

/****************************************************************************/
/*  MySQL/PostgreSQL specific global variables.  Used for Logzilla & Snort  */
/*  Database support							    */
/****************************************************************************/

#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

char dbusername[MAXUSER];
char dbpassword[MAXUSER];
char dbname[MAXDBNAME];
char dbhost[MAXHOST];

int  dbtype;
int  threaddbc=0;
int  threadmaxdbc=0;
int  maxdb_threads;

char logzilla_user[MAXUSER];
char logzilla_password[MAXPASS];
char logzilla_dbname[MAXDBNAME];
char logzilla_dbhost[MAXHOST];

int  logzilla_log;
int  logzilla_dbtype;
int  threadlogzillac=0;
int  threadmaxlogzillac=0;
int  max_logzilla_threads;


pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logzilla_mutex = PTHREAD_MUTEX_INITIALIZER;

#endif

/****************************************************************************/
/* libesmtp global variables.  For e-mail/SMTP support.			    */
/****************************************************************************/

#ifdef HAVE_LIBESMTP

char sagan_esmtp_from[ESMTPFROM];
char sagan_esmtp_to[ESMTPTO];
char sagan_esmtp_server[ESMTPSERVER];

int  max_email_threads;
int  min_email_priority=0;
int  threadmaxemailc=0;
int  threademailc=0;

pthread_mutex_t email_mutex = PTHREAD_MUTEX_INITIALIZER;


#endif 

/* Command line options */

const struct option long_options[] = {
	{ "help",         no_argument,          NULL,   'h' },
	{ "debug", 	  no_argument, 		NULL, 	'd' },
	{ "daemon", 	  no_argument,		NULL,	'D' },
	{ "program", 	  no_argument,		NULL, 	'p' },
	{ "devdebug", 	  no_argument, 		NULL,   'Z' },
	{ "user",	  required_argument,	NULL,	'U' },
	{ "chroot", 	  no_argument, 		NULL, 	'c' }, 
	{0, 0, 0, 0}

};

static const char *short_options =
":pdDhZUc";

int option_index = 0;

/* ######################################################################## */
/* Start of main() thread!
 * ######################################################################## */

int main(int argc, char **argv) {

/****************************************************************************/
/* MySQL / PostgreSQL (snort/logzilla) local variables			    */
/****************************************************************************/

#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

int  endianchk;

unsigned long long cid = 0;

struct db_args *db_args = NULL;
struct logzilla_thread_args *logzilla_thread_args = NULL;

pthread_t threaddb_id[MAX_THREADS];
pthread_attr_t thread_db_attr;
pthread_mutex_init(&db_mutex, NULL);

pthread_attr_init(&thread_db_attr);
pthread_attr_setdetachstate(&thread_db_attr,  PTHREAD_CREATE_DETACHED);

pthread_t threadlogzilla_id[MAX_THREADS];
pthread_attr_t thread_logzilla_attr;
pthread_mutex_init(&logzilla_mutex, NULL);

pthread_attr_init(&thread_logzilla_attr);
pthread_attr_setdetachstate(&thread_logzilla_attr,  PTHREAD_CREATE_DETACHED);

endianchk = checkendian();	// Needed for Snort output

#endif

/****************************************************************************/
/* libesmtp (SMTP/e-mail) local variables				    */
/****************************************************************************/

#ifdef HAVE_LIBESMTP
struct email_thread_args *email_thread_args = NULL;

pthread_t threademail_id[MAX_THREADS];
pthread_attr_t thread_email_attr;
pthread_mutex_init(&email_mutex, NULL);

pthread_attr_init(&thread_email_attr);
pthread_attr_setdetachstate(&thread_email_attr,  PTHREAD_CREATE_DETACHED);

#endif

/****************************************************************************/
/* Various local variables						    */
/****************************************************************************/

/* Block all signals,  we create a signal handling thread */

sigset_t signal_set;
pthread_t sig_thread;
sigfillset( &signal_set );
pthread_sigmask( SIG_BLOCK, &signal_set, NULL );

pthread_t key_thread;
pthread_attr_t key_thread_attr;
pthread_attr_init(&key_thread_attr);
pthread_attr_setdetachstate(&key_thread_attr,  PTHREAD_CREATE_DETACHED);

pthread_t threadext_id[MAX_THREADS];
pthread_attr_t thread_ext_attr;
pthread_mutex_init(&ext_mutex, NULL);
pthread_attr_init(&thread_ext_attr);
pthread_attr_setdetachstate(&thread_ext_attr,  PTHREAD_CREATE_DETACHED);

char *ip_src = NULL;
char *ip_dst = NULL;
char  ip_srctmp[MAXIP];
char  ip_dsttmp[MAXIP];
int  src_port;
int  thresh_count_by_src=0;
int  thresh_count_by_dst=0;
int  thresh_flag=0;
int  thresh_log_flag=0;
char  timet[20];

struct thresh_by_src *threshbysrc = NULL;
struct thresh_by_dst *threshbydst = NULL;

unsigned long long thresh_oldtime_src;

char *pattern;
char *prgpattern;

char fip[MAXIP];

char *syslog_host;
char  syslog_hosttmp[MAXHOST];

char *syslog_facility;
char syslog_facilitytmp[MAXFACILITY];

char *syslog_priority;
char syslog_prioritytmp[MAXPRIORITY];

char *syslog_level;
char syslog_leveltmp[MAXLEVEL];

char *syslog_tag;
char syslog_tagtmp[MAXTAG];

char *syslog_date;
char syslog_datetmp[MAXDATE];

char *syslog_time;
char syslog_timetmp[MAXTIME];

char *syslog_program;
char syslog_programtmp[MAXPROGRAM];


char *syslog_msg;
char  syslog_msg_origtmp[MAX_SYSLOGMSG];

struct ext_thread_args *ext_thread_args = NULL;

pcre *re;

const char *error;

int erroffset;
int rc=0;
int option;

int ovector[OVECCOUNT];

char syslogstring[MAX_SYSLOGMSG];
char sysmsg[MAX_SYSLOGMSG];
char syslogtmp[2];
char c;

char *ptmp;
char *tok;
char *tok2;

/* For the ruleset */

char *content;
char *program;
char *facility;
char *syspri;
char *level;
char *tag;
char tmpbuf[128];

char *syslog_msg_case;
char *s_content_case;

int i;
int fd=0;
int b;
int z;
int match=0;
int pcrematch=0;
int key;

time_t t;
struct tm *now;

if ( pthread_create( &sig_thread, NULL, (void *)sig_handler, &sig_thread_args )) { 
        removelockfile();
        sagan_log(1, "[%s, line %d] Error creating signa handler thread.", __FILE__, __LINE__);
        }


/* Get command line arg's */
while ((c = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) { 
   
   switch(c) {
           
	   if (c == -1) break;

           case 'h':
	   sagan_usage();
	   exit(0);
	   break;
	   
	   case 'd':
	   sagan_log(0, "Debug enabled");
	   debug=1;
	   break;
          
	   case 'D':
	   daemonize=1;
	   break;

	   case 'Z':
	   devdebug=1;
	   break;

	   case 'U':
	   runas=optarg;
	   break;

	   case 'p':
	   programmode=1;
	   break;

	   case 'c':
	   dochroot=1;
	   break;

	  default:
          fprintf(stderr, "Invalid argument! See below for command line switches.\n");
          sagan_usage();
	  exit(0);
	  break;
  	  }
}

/* create the signal handling thread */

sig_thread_args[0].daemonize = daemonize;

sagan_log(0, "Sagan version %s is firing up!", VERSION);
load_config();
sagan_log(0, "Configuration file loaded and %d rules loaded.", rulecount);
sagan_log(0, "---------------------------------------------------------------------------");

/* Allocate memory for external program thread structure */


if ( strcmp(sagan_extern, "" )) { 
ext_thread_args = malloc(MAX_THREADS * sizeof(struct ext_thread_args));
sagan_log(0, "Max external threads : %d", max_ext_threads);
}

/* Allocate memory for libesmtp thread struct */

#ifdef HAVE_LIBESMTP
if ( strcmp(sagan_esmtp_server, "" )) { 
email_thread_args = malloc(MAX_THREADS * sizeof(struct email_thread_args));
sagan_log(0, "Max SMTP threads     : %d", max_email_threads);
}
#endif

#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)
if ( logzilla_dbtype != 0 ) { 
logzilla_thread_args = malloc(MAX_THREADS * sizeof(struct logzilla_thread_args));
sagan_log(0, "Max Logzilla threads : %d", max_logzilla_threads);
logzilla_db_connect();
}
#endif 


#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

sig_thread_args[0].daemonize = daemonize;

if ( dbtype != 0 ) { 

/* Allocate memory for DB thread structure */

db_args = malloc(MAX_THREADS * sizeof(struct db_args)); 
sagan_log(0, "Max database threads : %d", maxdb_threads);


db_connect();
get_sensor_id( sagan_hostname, sagan_interface, sagan_filter, sagan_detail, dbtype);
sagan_log(0, "Sensor ID            : %d", sensor_id);
cid = get_cid( sensor_id, dbtype );
cid++;
sigcid=cid;
sagan_log(0, "Next CID             : %llu", cid);

}
#endif

sagan_log(0, "");

if ( fifoi == 0 ) { 
   if ( programmode == 0 ) sagan_log(0, "No FIFO option found,  assuming syslog-ng 'program' mode.");
   } else { 
   sagan_log(0, "Opening syslog FIFO (%s)", fifo);
   fd = open(fifo, O_RDONLY); 
   }

droppriv(runas);	/* Drop priv's here,  so we can open FIFO even if owned by 'root' */


sagan_log(0, "");
sagan_log(0, " ,-._,-. 	-*> Sagan! <*-");
sagan_log(0, " \\/)\"(\\/	Version %s", VERSION);
sagan_log(0, "  (_o_)  	By Champ Clark III & The Softwink Team: http://www.softwink.com");
sagan_log(0, "  /   \\/)	Copyright (C) 2009-2010 Softwink, Inc., et al.");
sagan_log(0, " (|| ||) 	Using PCRE version: %s", pcre_version());
sagan_log(0, "  oo-oo     Sagan is processing events.....");
sagan_log(0, "");

/* Become a daemon if requested */

if (daemonize)
{

/* Unblock signals so the daemon can catch them */

sigfillset(&signal_set);
pthread_sigmask(SIG_UNBLOCK, &signal_set, NULL );

signal (SIGHUP,  &sig_handler );
signal (SIGINT,  &sig_handler );
signal (SIGQUIT, &sig_handler );
signal (SIGTERM, &sig_handler );
signal (SIGABRT, &sig_handler );
signal (SIGSEGV, &sig_handler );
signal (SIGUSR1, &sig_handler );

sagan_log(0, "Becoming a daemon!");

pid_t pid = 0;
setsid();
pid = fork();
if (pid == 0) {} else { exit(0); }
} 

/* We don't want the key_handler() if we're in program/daemon mode! */

if (!daemonize && !program) { 

if (pthread_create( &key_thread, NULL, (void *)key_handler, NULL )) { ;
	removelockfile();
	sagan_log(1, "[%s, line %d] Error creating key_handler thread.", __FILE__, __LINE__);
	}

}


/* Check lock file _after_ thread.  If you don't it'll retreive the wrong pid
 * and incorrectly believe there is a stale lock file if --daemon */

checklockfile();

while(1) { 

		if ( fifoi == 1 ) { 

		pthread_mutex_lock( &general_mutex );

                if(fd < 0) {
		        removelockfile();
			sagan_log(1, "[%s, line %d] Error opening in FIFO! %s (Errno: %d)", __FILE__, __LINE__, fifo, errno);
                }
                i = read(fd, &c, 1);
                if(i < 0) {
		        removelockfile();
                        sagan_log(1, "[%s, line %d] Error reading FIFO! %s (Errno: %d)", __FILE__, __LINE__, fifo, errno);
                        }

                snprintf(syslogtmp, sizeof(syslogtmp), "%c", c);
                strncat(syslogstring, syslogtmp, 1); 

		pthread_mutex_unlock( &general_mutex );

		} else { 

		pthread_mutex_lock( &general_mutex );

		if (!fgets(syslogstring, sizeof(syslogstring), stdin)) { 
		   sagan_log(0, "Dropped input in 'program' mode!");
		   sagandrop++;
		}
		pthread_mutex_unlock( &general_mutex );
		}

		if ( c == '\n' || c == '\r' || fifoi == 0  ) 
                {

		sagantotal++;

		/* We have to check for values be "NULL" in the event that
		 * the program generating the message did so incorrectly
		 * *cough* Asterisk *cough*.  So we do a little checking 
		 * here.  If we 'see' a bad valid,  we attempt to correct 
		 * it */

		syslog_host = strtok_r(syslogstring, "|", &tok);
		if (syslog_host == NULL ) { 
		   syslog_host = "SAGAN: HOST ERROR"; 
		   sagan_log(0, "Sagan received a malformed 'host'");
		   }
		
		syslog_facility=strtok_r(NULL, "|", &tok);
		if ( syslog_facility == NULL ) { 
		   syslog_facility = "SAGAN: FACILITY ERROR";
		   sagan_log(0, "Sagan received a malformed 'facility'");
		   }

                syslog_priority=strtok_r(NULL, "|", &tok);
		if ( syslog_priority == NULL ) { 
		   syslog_priority = "SAGAN: PRIORITY ERROR";
		   sagan_log(0, "Sagan received a malformed 'priority'");
		   }

                syslog_level=strtok_r(NULL, "|", &tok);
		if ( syslog_level == NULL ) { 
		   syslog_level = "SAGAN: LEVEL ERROR";
		   sagan_log(0, "Sagan received a malformed 'priority'");
		   }

                syslog_tag=strtok_r(NULL, "|", &tok);
                if ( syslog_tag == NULL ) {
                   syslog_tag = "SAGAN: TAG ERROR";
                   sagan_log(0, "Sagan received a malformed 'tag'");
                   }

                syslog_date=strtok_r(NULL, "|", &tok);
                if ( syslog_date == NULL ) {
                   syslog_date = "SAGAN: DATE ERROR";
                   sagan_log(0, "Sagan received a malformed 'date'");
                   }

                syslog_time=strtok_r(NULL, "|", &tok);
                if ( syslog_time == NULL ) {
                   syslog_time = "SAGAN: TIME ERROR";
                   sagan_log(0, "Sagan received a malformed 'time'");
                   }

                syslog_program=strtok_r(NULL, "|", &tok);
                if ( syslog_program == NULL ) {
                   syslog_program = "SAGAN: PROGRAM ERROR";
                   sagan_log(0, "Sagan received a malformed 'program'");
                   }

                syslog_msg=strtok_r(NULL, "|", &tok);
                if ( syslog_msg == NULL ) {
                   syslog_msg = "SAGAN: MESSAGE ERROR";
                   sagan_log(0, "Sagan received a malformed 'message'");
                   }


		if (debug) { 
		printf("= Host: [%s] ", syslog_host);
		printf("Facility: [%s] ", syslog_facility);
		printf("Pri: [%s] ", syslog_priority);
		printf("Level: [%s] ", syslog_level);
		printf("Tag: [%s] ", syslog_tag);
		printf("Date: [%s] ", syslog_date);
		printf("Time [%s] ", syslog_time);
		printf("Program: [%s] ", syslog_program);
		printf("Msg: [%s]\n", syslog_msg);
		fflush(stdout);
		}


   pthread_mutex_lock( &general_mutex );

   snprintf(syslog_hosttmp, sizeof(syslog_hosttmp), "%s", syslog_host);
   snprintf(syslog_programtmp, sizeof(syslog_programtmp), "%s", syslog_program);
   snprintf(sysmsg, sizeof(sysmsg), "%s", syslog_msg);
   snprintf(syslog_datetmp, sizeof(syslog_datetmp), "%s", syslog_date);
   snprintf(syslog_timetmp, sizeof(syslog_timetmp), "%s", syslog_time);
   snprintf(syslog_facilitytmp, sizeof(syslog_facilitytmp), "%s", syslog_facility);
   snprintf(syslog_prioritytmp, sizeof(syslog_prioritytmp), "%s", syslog_priority);
   snprintf(syslog_tagtmp, sizeof(syslog_tagtmp), "%s", syslog_tag);
   snprintf(syslog_leveltmp, sizeof(syslog_leveltmp), "%s", syslog_level);

   pthread_mutex_unlock( &general_mutex );

/***************************************************************************/
/* Logzilla _FULL_ logging support.  This logs everything!		   */
/***************************************************************************/

#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

                if ( logzilla_dbtype != 0 && logzilla_log == 1 ) {

                   if ( threadlogzillac < max_logzilla_threads) {

                      pthread_mutex_lock( &logzilla_mutex );
                      threadlogzillac++;
                      threadid++;
		      if ( threadid >= MAX_THREADS ) threadid=0;
		      pthread_mutex_unlock( &logzilla_mutex );

                      if ( threadlogzillac > threadmaxlogzillac ) threadmaxlogzillac=threadlogzillac;

		      logzilla_thread_args[threadid].host=syslog_hosttmp;
                      logzilla_thread_args[threadid].facility=syslog_facilitytmp;
                      logzilla_thread_args[threadid].priority=syslog_prioritytmp;
                      logzilla_thread_args[threadid].level=syslog_leveltmp;
                      logzilla_thread_args[threadid].tag=syslog_tagtmp;
                      logzilla_thread_args[threadid].date=syslog_datetmp;
                      logzilla_thread_args[threadid].time=syslog_timetmp;
                      logzilla_thread_args[threadid].program=syslog_programtmp;
		      logzilla_thread_args[threadid].msg=sysmsg;

                     if ( pthread_create( threadlogzilla_id, &thread_logzilla_attr, (void *)sagan_logzilla_thread, &logzilla_thread_args[threadid]) ) {
                          removelockfile();
                          sagan_log(1, "[%s, line %d] Error creating database thread.", __FILE__, __LINE__);
       		          }
	                	} else {
                   		sagandrop++;
                   		sagan_log(0, "Logzilla thread handler: Out of threads");
                  	  }
                  }

#endif

		/* Search for matches */

		/* First we search for 'program' and such.   This way,  we don't waste CPU
		 * time with pcre/content.  */

		for(b=0;b<rulecount;b++) {

                match = 0; program=""; facility=""; syspri=""; level=""; tag=""; content="";

                if ( strcmp(rulestruct[b].s_program, "" )) {
                   snprintf(tmpbuf, sizeof(tmpbuf), "%s", rulestruct[b].s_program);
                   ptmp = strtok_r(tmpbuf, "|", &tok2);
                   match=1;
                   while ( ptmp != NULL ) {
                       if (!strcmp(ptmp, syslog_programtmp)) match=0;
                       ptmp = strtok_r(NULL, "|", &tok2);
                       }
                }

                if ( strcmp(rulestruct[b].s_facility, "" )) {
                   snprintf(tmpbuf, sizeof(tmpbuf), "%s", rulestruct[b].s_facility);
                   ptmp = strtok_r(tmpbuf, "|", &tok2);
                   match=1;
                   while ( ptmp != NULL ) {
                      if (!strcmp(ptmp, syslog_facilitytmp)) match=0;
                      ptmp = strtok_r(NULL, "|", &tok2);
                      }
                }

                if ( strcmp(rulestruct[b].s_syspri, "" )) {
                   snprintf(tmpbuf, sizeof(tmpbuf), "%s", rulestruct[b].s_syspri);
                   ptmp = strtok_r(tmpbuf, "|", &tok2);
                   match=1;
                   while ( ptmp != NULL ) {
                      if (!strcmp(ptmp, syslog_prioritytmp)) match=0;
                      ptmp = strtok_r(NULL, "|", &tok2);
                      }
                  }

                if ( strcmp(rulestruct[b].s_level, "" )) {
                   snprintf(tmpbuf, sizeof(tmpbuf), "%s", rulestruct[b].s_level);
                   ptmp = strtok_r(tmpbuf, "|", &tok2);
                   match=1;
                   while ( ptmp != NULL ) {
                      if (!strcmp(ptmp, syslog_leveltmp)) match=0;
                       ptmp = strtok_r(NULL, "|", &tok2);
                       }
                   }

                if ( strcmp(rulestruct[b].s_tag, "" )) {
                   snprintf(tmpbuf, sizeof(tmpbuf), "%s", rulestruct[b].s_tag);
                   ptmp = strtok_r(tmpbuf, "|", &tok2);
                   match=1;
                   while ( ptmp != NULL ) {
                      if (!strcmp(ptmp, syslog_tagtmp)) match=0;
                      ptmp = strtok_r(NULL, "|", &tok2);
                      }
                }

		/* If there has been a match above,  or NULL on all,  then we continue with 
		 * PCRE/content search */

		/* Search via strstr (content:) */

		if ( match == 0 ) { 

		if ( strcmp(rulestruct[b].s_content[0], "" )) { 

		for(z=0; z<rulestruct[b].content_count; z++) {
		 
		   /* If case insensitive */
		   if ( rulestruct[b].s_nocase == 1 ) {
		      snprintf(syslog_msg_origtmp,  sizeof(syslog_msg_origtmp), "%s", syslog_msg);
		      syslog_msg_case=syslog_msg_origtmp;
		      s_content_case=rulestruct[b].s_content[z];
		      toupperc(syslog_msg_case);
		      toupperc(s_content_case); 
		      if (strstr(syslog_msg_case, s_content_case )) pcrematch++;   // rc=1;
		      } else { 

		   /* If case sensitive */
		   if (strstr(syslog_msg, rulestruct[b].s_content[z] )) pcrematch++;  // rc=1;
		   }
		  }
		 }
	      
	       
	       	/* Search via PCRE */

                if ( strcmp(rulestruct[b].s_pcre[0], "" )) {
	
		   for(z=0; z<rulestruct[b].pcre_count; z++) {
 		      pattern=rulestruct[b].s_pcre[z];
		      option=rulestruct[b].s_pcreoptions[z];

	                /* Compile regexp string */

       		re =  pcre_compile( pattern, option, &error, &erroffset, NULL );
		
		/* pcre_study() here? */

                if ( re == NULL ) {
                removelockfile();
                sagan_log(1, "[%s, line %d] PCRE failure at %d: %s", __FILE__, __LINE__, erroffset, error);
                }

                rc = pcre_exec( re, NULL, syslog_msg, (int)strlen(syslog_msg), 0, 0, ovector, OVECCOUNT);
                pcre_free(re);

                }  /* End of pcre if */

                if ( rc == 1 ) {
                   pcrematch++;
                   }
                  }
		
		} /* End of content: & pcre */
	
		/* if you got match */

		if ( pcrematch == rulestruct[b].pcre_count + rulestruct[b].content_count )
		   {
		
		   /*
		    * Signature should have a classification.  If it does,  we set the priority of
		    * the alert here.  
		    */

		    for(i=0; i<classcount;i++) { 
			   if (!strstr(classstruct[i].s_shortname, rulestruct[b].s_classtype)) {
				   fpri=classstruct[i].s_priority;
			   	}
		   	}

		/* If ruleset has a 'priority' statement,  then that will over ride classificatons */
		/* Note: Don't have 'classification' or 'priority'?  Then it gets set to the defaultpri */

                if ( atoi(rulestruct[b].s_pri) != 0 ) fpri = atoi(rulestruct[b].s_pri);


		   if ( match == 0 ) { 

		   saganfound++;

		   /* Search message for possible IP addresses */

		   if ( sagan_find_ip == 1 ) 
		      {
		      snprintf(fip, sizeof(fip), "%s", findipinmsg(syslog_msg));
		         
			 if (strcmp(fip,"0")) 
                            {
                            ip_src = fip; ip_dst = syslog_host;
                            } else { 
                            ip_src = syslog_host; ip_dst = sagan_host;
                            }
                      } else { 
                      ip_src = syslog_host; ip_dst = sagan_host;
                      }

		   if ( rulestruct[b].s_find_port == 1) { 
		   src_port = getport(syslog_msg);
		   } else { 
		   src_port = atoi(sagan_port);
		   }

		   snprintf(ip_srctmp, sizeof(ip_srctmp), "%s", ip_src);
		   snprintf(ip_dsttmp, sizeof(ip_dsttmp), "%s", ip_dst);

		   thresh_log_flag = 0;

		   /*********************************************************/
		   /* Thresh holding                                        */
		   /*********************************************************/

		   if ( rulestruct[b].threshold_type != 0 ) { 

		      t = time(NULL);
		      now=localtime(&t);
		      strftime(timet, sizeof(timet), "%s",  now);

		      /* Thresholding by source IP address */
		      
		      if ( rulestruct[b].threshold_src_or_dst == 1 ) { 
		         thresh_flag = 0;
	
			 /* Check array for matching src / sid */

			 for (i = 0; i < thresh_count_by_src; i++ ) { 
			     if (!strcmp( threshbysrc[i].ipsrc, ip_src ) && !strcmp(threshbysrc[i].sid, rulestruct[b].s_sid )) { 
			        thresh_flag=1;
				threshbysrc[i].count++;
				thresh_oldtime_src = atol(timet) - threshbysrc[i].utime;
				threshbysrc[i].utime = atol(timet);
				if ( thresh_oldtime_src > rulestruct[b].threshold_seconds ) {
				   threshbysrc[i].count=1;
				   threshbysrc[i].utime = atol(timet);
				   thresh_log_flag=0;
				   }

				if ( rulestruct[b].threshold_count < threshbysrc[i].count ) 
					{ 
					thresh_log_flag = 1;
					sagan_log(0, "Threshold SID %s by source IP address. [%s]", threshbysrc[i].sid, ip_src);
					threshold_total++;
					}
  					
			     }
			 }
	
			 /* If not found,  add it to the array */
			
			 if ( thresh_flag == 0 ) { 
			    threshbysrc = (thresh_by_src *) realloc(threshbysrc, (thresh_count_by_src+1) * sizeof(thresh_by_src));
                            snprintf(threshbysrc[thresh_count_by_src].ipsrc, sizeof(threshbysrc[thresh_count_by_src].ipsrc), "%s", ip_src);
			    snprintf(threshbysrc[thresh_count_by_src].sid, sizeof(threshbysrc[thresh_count_by_src].sid), "%s", rulestruct[b].s_sid );
			    threshbysrc[thresh_count_by_src].count = 1;
			    threshbysrc[thresh_count_by_src].utime = atol(timet);
			    thresh_count_by_src++;
			    }
			 }

		      /* Thresholding by destination IP address */

                      if ( rulestruct[b].threshold_src_or_dst == 2 ) {
                         thresh_flag = 0;
       
                         /* Check array for matching src / sid */

                         for (i = 0; i < thresh_count_by_dst; i++ ) {
                             if (!strcmp( threshbydst[i].ipdst, ip_dst ) && !strcmp(threshbydst[i].sid, rulestruct[b].s_sid )) {
                                thresh_flag=1;
                                threshbydst[i].count++;
                                thresh_oldtime_src = atol(timet) - threshbydst[i].utime;
                                threshbydst[i].utime = atol(timet);
                                if ( thresh_oldtime_src > rulestruct[b].threshold_seconds ) {
                                   threshbydst[i].count=1;
                                   threshbydst[i].utime = atol(timet);
                                   thresh_log_flag=0;
                                   }

                                if ( rulestruct[b].threshold_count < threshbydst[i].count ) 
				   {
				   thresh_log_flag = 1;
				   sagan_log(0, "Threshold SID %s by source IP address. [%s]", threshbysrc[i].sid, ip_dst);
				   threshold_total++;
				   }
                             }
                         }

                         /* If not found,  add it to the array */

                         if ( thresh_flag == 0 ) {
                            threshbydst = (thresh_by_dst *) realloc(threshbydst, (thresh_count_by_dst+1) * sizeof(thresh_by_dst));
                            snprintf(threshbydst[thresh_count_by_dst].ipdst, sizeof(threshbydst[thresh_count_by_dst].ipdst), "%s", ip_dst);
                            snprintf(threshbydst[thresh_count_by_dst].sid, sizeof(threshbydst[thresh_count_by_dst].sid), "%s", rulestruct[b].s_sid );
                            threshbydst[thresh_count_by_dst].count = 1;
                            threshbydst[thresh_count_by_dst].utime = atol(timet);
                            thresh_count_by_dst++;
                            }
                         }

		      }			/* End of thresholding */
		      

		   /* alert log file */
		   
		   if ( thresh_log_flag == 0 ) sagan_alert( rulestruct[b].s_sid, rulestruct[b].s_msg, rulestruct[b].s_classtype, fpri, syslog_datetmp, syslog_timetmp, ip_src, ip_dst, syslog_facilitytmp, syslog_leveltmp, rulestruct[b].dst_port, src_port );


#ifdef HAVE_LIBESMTP

/****************************************************************************/
/* libesmtp thread call (SMTP/email)                                        */
/****************************************************************************/


		if ( strcmp(sagan_esmtp_server, "") && thresh_log_flag == 0 ) { 
		  
		  /* E-mail only if over min_email_priority */ 

		  if ( min_email_priority >= fpri || min_email_priority == 0 ) { 

		   if ( threademailc < max_email_threads ) { 
		  
		      pthread_mutex_lock ( &email_mutex );
		      threademailc++;
		      threadid++;
		      if ( threadid >= MAX_THREADS ) threadid=0;
		      pthread_mutex_unlock( &email_mutex );
	 

		      if ( threademailc > threadmaxemailc ) threadmaxemailc=threademailc;
		   
                   	  email_thread_args[threadid].sid = rulestruct[b].s_sid;
                   	  email_thread_args[threadid].msg = rulestruct[b].s_msg;
                   	  email_thread_args[threadid].classtype = rulestruct[b].s_classtype;
                   	  email_thread_args[threadid].pri = fpri;
                   	  email_thread_args[threadid].date = syslog_datetmp;
                   	  email_thread_args[threadid].time = syslog_timetmp;
                   	  email_thread_args[threadid].ip_src = ip_srctmp;
                   	  email_thread_args[threadid].ip_dst = ip_dsttmp;
                   	  email_thread_args[threadid].facility = syslog_facilitytmp;
                   	  email_thread_args[threadid].fpri = syslog_leveltmp;
		   	  email_thread_args[threadid].sysmsg = sysmsg;
		    	  email_thread_args[threadid].dst_port = rulestruct[b].dst_port;
		   	  email_thread_args[threadid].src_port = src_port;
	
                if ( pthread_create( threademail_id, &thread_email_attr, (void *)sagan_esmtp_thread, &email_thread_args[threadid] ) ) {
		      removelockfile();
                      sagan_log(1, "[%s, line %d] Error creating SMTP thread", __FILE__, __LINE__);
                      }
				} else { 
				sagandrop++;
				sagan_log(0, "SMTP thread call handler: Out of threads\n");
		      }
		}
	}
#endif
		
/****************************************************************************/
/* External program thread call                                             */
/****************************************************************************/

		if ( strcmp(sagan_extern, "" ) && thresh_log_flag == 0 ) { 
		   
		   if ( threadextc < max_ext_threads ) { 
		   pthread_mutex_lock ( &ext_mutex );
		   threadextc++;
		   threadid++;
		   if ( threadid >= MAX_THREADS ) threadid=0;
		   pthread_mutex_unlock( &ext_mutex );
		   
		   if ( threadextc > threadmaxextc ) threadmaxextc=threadextc;
		  
                   ext_thread_args[threadid].sid = rulestruct[b].s_sid;
                   ext_thread_args[threadid].msg = rulestruct[b].s_msg;
                   ext_thread_args[threadid].classtype = rulestruct[b].s_classtype;
                   ext_thread_args[threadid].pri = fpri;
                   ext_thread_args[threadid].date = syslog_datetmp;
                   ext_thread_args[threadid].time = syslog_timetmp;
 		   ext_thread_args[threadid].ip_src = ip_srctmp;
                   ext_thread_args[threadid].ip_dst = ip_dsttmp;
                   ext_thread_args[threadid].facility = syslog_facilitytmp;
                   ext_thread_args[threadid].fpri = syslog_leveltmp;
		   ext_thread_args[threadid].sysmsg = sysmsg;
		   ext_thread_args[threadid].dst_port = rulestruct[b].dst_port;
		   ext_thread_args[threadid].src_port = src_port;

		   if ( pthread_create( threadext_id, &thread_ext_attr, (void *)sagan_ext_thread, &ext_thread_args[threadid] ) ) { 
		     removelockfile();
		     sagan_log(1, "[%s, line %d] Error creating external call thread", __FILE__, __LINE__);
		      }

		   } else {

		   sagandrop++; 
		   sagan_log(0, "External thread call handler: Out of threads\n");

		   }
                 }



/****************************************************************************/
/* Logzilla,  alert only,  thread call                                      */
/****************************************************************************/

#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

		if ( logzilla_dbtype != 0 && thresh_log_flag == 0 && logzilla_log == 2 ) { 
		   
		   if ( threadlogzillac < max_logzilla_threads) { 
		      
		      pthread_mutex_lock( &logzilla_mutex );
		      threadlogzillac++;
		      threadid++;
		      if ( threadid >= MAX_THREADS ) threadid=0;
		      pthread_mutex_unlock( &logzilla_mutex );
		      
		      if ( threadlogzillac > threadmaxlogzillac ) threadmaxlogzillac=threadlogzillac;

		      logzilla_thread_args[threadid].host=ip_srctmp;
		      logzilla_thread_args[threadid].facility=syslog_facilitytmp;
		      logzilla_thread_args[threadid].priority=syslog_prioritytmp;
		      logzilla_thread_args[threadid].level=syslog_leveltmp;
		      logzilla_thread_args[threadid].tag=syslog_tagtmp;
		      logzilla_thread_args[threadid].date=syslog_datetmp;
		      logzilla_thread_args[threadid].time=syslog_timetmp;
		      logzilla_thread_args[threadid].program=syslog_programtmp;
		      logzilla_thread_args[threadid].msg=sysmsg;
		      


                     if ( pthread_create( threadlogzilla_id, &thread_logzilla_attr, (void *)sagan_logzilla_thread, &logzilla_thread_args[threadid]) ) {
                          removelockfile();
                          sagan_log(1, "[%s, line %d] Error creating database thread.", __FILE__, __LINE__);
		   }

		} else { 
		   
		   sagandrop++;
		   sagan_log(0, "Logzilla thread handler: Out of threads");
		  }
	  }

#endif

/****************************************************************************/
/* Snort database thread call                                               */
/****************************************************************************/


#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

 		if ( dbtype != 0 && thresh_log_flag == 0 ) { 

		   pthread_mutex_lock( &db_mutex );
                   threaddbc++;
                   threadid++;
		   if ( threadid >= MAX_THREADS ) threadid=0;
		   pthread_mutex_unlock( &db_mutex );

	  	if ( threaddbc < maxdb_threads ) { 

		   if ( threaddbc > threadmaxdbc ) threadmaxdbc=threaddbc;
                
                   pthread_mutex_lock( &db_mutex );
		   cid++; 
		   sigcid=cid;
		   pthread_mutex_unlock( &db_mutex );

		   db_args[threadid].ip_src=ip_srctmp;
		   db_args[threadid].ip_dst=ip_dsttmp;
                   db_args[threadid].found=b;
                   db_args[threadid].pri=fpri;
		   db_args[threadid].message=sysmsg;
		   db_args[threadid].cid=cid;
		   db_args[threadid].endian=endianchk;
		   db_args[threadid].dst_port = rulestruct[b].dst_port;
		   db_args[threadid].src_port = src_port;

		if ( pthread_create( threaddb_id, &thread_db_attr, (void *)sagan_db_thread, &db_args[threadid]) ) { 
		    removelockfile();
		    sagan_log(1, "[%s, line %d] Error creating database thread.", __FILE__, __LINE__);
		    }

		    } else { 

		    sagandrop++;
		    sagan_log(0, "Snort database thread handler: Out of threads");
	        }
	   }
#endif
	 	    
        }
     }

     match=0;  /* Reset match! */
     pcrematch=0;
     fpri=0;
     rc=0;
  }

  pthread_mutex_lock( &general_mutex );
  strlcpy(syslogstring, "", sizeof(syslogstring));
  strlcpy(syslogtmp, "", sizeof(syslogtmp));
  pthread_mutex_unlock( &general_mutex );
  }
 }
} /* end of main */


/***************************************************************************/
/* Snort specific thread code                                              */
/***************************************************************************/

#if defined(HAVE_LIBMYSQLCLIENT_R) || defined(HAVE_LIBPQ)

void *sagan_db_thread( void *sthreadargs ) { 

struct db_thread_args * targs = (struct db_thread_args *) sthreadargs;

int sig_sid; 
int i;
char *hex_data = NULL;                                         

sig_sid = get_sig_sid(rulestruct[targs->found].s_msg, rulestruct[targs->found].s_rev,  rulestruct[targs->found].s_sid, rulestruct[targs->found].s_classtype,  targs->pri , dbtype ); 
insert_event( sensor_id, targs->cid, sig_sid, dbtype);

insert_hdr(sensor_id, targs->cid, targs->ip_src, targs->ip_dst, rulestruct[targs->found].ip_proto, targs->endian, dbtype, targs->dst_port,targs->src_port );

pthread_mutex_lock( &db_mutex );			
hex_data = fasthex(targs->message, strlen(targs->message));
pthread_mutex_unlock( &db_mutex );

insert_payload ( sensor_id, targs->cid, hex_data, dbtype ) ;

for (i = 0; i < rulestruct[targs->found].ref_count; i++ ) {  
   query_reference( rulestruct[targs->found].s_reference[i], rulestruct[targs->found].s_sid, sig_sid, i );
   }

pthread_mutex_lock( &db_mutex );
threaddbc--;
pthread_mutex_unlock( &db_mutex );

pthread_exit(NULL);
}
#endif
