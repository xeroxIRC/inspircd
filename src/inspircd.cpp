/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  Inspire is copyright (C) 2002-2005 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *           	  <Craig@chatspike.net>
 *     
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

/* Now with added unF! ;) */

using namespace std;

#include "inspircd_config.h"
#include "inspircd.h"
#include "inspircd_io.h"
#include "inspircd_util.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>

#ifdef USE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#endif

#ifdef USE_EPOLL
#include <sys/epoll.h>
#define EP_DELAY 5
#endif

#include <time.h>
#include <string>
#ifdef GCC3
#include <ext/hash_map>
#else
#include <hash_map>
#endif
#include <map>
#include <sstream>
#include <vector>
#include <deque>
#include <sched.h>
#ifdef THREADED_DNS
#include <pthread.h>
#endif
#include "users.h"
#include "ctables.h"
#include "globals.h"
#include "modules.h"
#include "dynamic.h"
#include "wildcard.h"
#include "message.h"
#include "mode.h"
#include "commands.h"
#include "xline.h"
#include "inspstring.h"
#include "dnsqueue.h"
#include "helperfuncs.h"
#include "hashcomp.h"
#include "socketengine.h"
#include "socket.h"
#include "dns.h"

int LogLevel = DEFAULT;
char ServerName[MAXBUF];
char Network[MAXBUF];
char ServerDesc[MAXBUF];
char AdminName[MAXBUF];
char AdminEmail[MAXBUF];
char AdminNick[MAXBUF];
char diepass[MAXBUF];
char restartpass[MAXBUF];
char motd[MAXBUF];
char rules[MAXBUF];
char list[MAXBUF];
char PrefixQuit[MAXBUF];
char DieValue[MAXBUF];
char DNSServer[MAXBUF];
char data[65536];
int debugging =  0;
int WHOWAS_STALE = 48; // default WHOWAS Entries last 2 days before they go 'stale'
int WHOWAS_MAX = 100;  // default 100 people maximum in the WHOWAS list
int DieDelay  =  5;
time_t startup_time = time(NULL);
int NetBufferSize = 10240;	// NetBufferSize used as the buffer size for all read() ops
int MaxConn = SOMAXCONN;	// size of accept() backlog (128 by default on *BSD)
unsigned int SoftLimit = MAXCLIENTS;
extern int MaxWhoResults;
time_t nb_start = 0;
int dns_timeout = 5;

char DisabledCommands[MAXBUF];

bool AllowHalfop = true;
bool AllowProtect = true;
bool AllowFounder = true;

extern std::vector<Module*> modules;
std::vector<std::string> module_names;
extern std::vector<ircd_module*> factory;

std::vector<InspSocket*> module_sockets;

extern int MODCOUNT;
int openSockfd[MAXSOCKS];
bool nofork = false;
bool unlimitcore = false;

time_t TIME = time(NULL), OLDTIME = time(NULL);

SocketEngine* SE = NULL;

bool has_been_netsplit = false;
extern std::vector<std::string> include_stack;

typedef nspace::hash_map<std::string, userrec*, nspace::hash<string>, irc::StrHashComp> user_hash;
typedef nspace::hash_map<std::string, chanrec*, nspace::hash<string>, irc::StrHashComp> chan_hash;
typedef nspace::hash_map<in_addr,string*, nspace::hash<in_addr>, irc::InAddr_HashComp> address_cache;
typedef nspace::hash_map<std::string, WhoWasUser*, nspace::hash<string>, irc::StrHashComp> whowas_hash;
typedef std::deque<command_t> command_table;
typedef std::map<std::string,time_t> autoconnects;
typedef std::vector<std::string> servernamelist;

// This table references users by file descriptor.
// its an array to make it VERY fast, as all lookups are referenced
// by an integer, meaning there is no need for a scan/search operation.
userrec* fd_ref_table[65536];

int statsAccept = 0, statsRefused = 0, statsUnknown = 0, statsCollisions = 0, statsDns = 0, statsDnsGood = 0, statsDnsBad = 0, statsConnects = 0, statsSent= 0, statsRecv = 0;

Server* MyServer = new Server;

FILE *log_file;

user_hash clientlist;
chan_hash chanlist;
whowas_hash whowas;
command_table cmdlist;
autoconnects autoconns;
file_cache MOTD;
file_cache RULES;
address_cache IP;

ClassVector Classes;
servernamelist servernames;

struct linger linger = { 0 };
char MyExecutable[1024];
int boundPortCount = 0;
int portCount = 0, SERVERportCount = 0, ports[MAXSOCKS];

char ModPath[MAXBUF];

/* prototypes */

int has_channel(userrec *u, chanrec *c);
int usercount(chanrec *c);
int usercount_i(chanrec *c);
char* Passwd(userrec *user);
bool IsDenied(userrec *user);
void AddWhoWas(userrec* u);

std::stringstream config_f(stringstream::in | stringstream::out);

std::vector<userrec*> all_opers;

char lowermap[255];

void AddOper(userrec* user)
{
	log(DEBUG,"Oper added to optimization list");
	all_opers.push_back(user);
}

void AddServerName(std::string servername)
{
	log(DEBUG,"Adding server name: %s",servername.c_str());
	for (servernamelist::iterator a = servernames.begin(); a < servernames.end(); a++)
	{
		if (*a == servername)
			return;
	}
	servernames.push_back(servername);
}

const char* FindServerNamePtr(std::string servername)
{
	for (servernamelist::iterator a = servernames.begin(); a < servernames.end(); a++)
	{
		if (*a == servername)
			return a->c_str();
	}
	AddServerName(servername);
	return FindServerNamePtr(servername);
}

void DeleteOper(userrec* user)
{
        for (std::vector<userrec*>::iterator a = all_opers.begin(); a < all_opers.end(); a++)
        {
                if (*a == user)
                {
                        log(DEBUG,"Oper removed from optimization list");
                        all_opers.erase(a);
                        return;
                }
        }
}

std::string GetRevision()
{
	char Revision[] = "$Revision$";
	char *s1 = Revision;
	char *savept;
	char *v2 = strtok_r(s1," ",&savept);
	s1 = savept;
	v2 = strtok_r(s1," ",&savept);
	s1 = savept;
	return std::string(v2);
}


std::string getservername()
{
	return ServerName;
}

std::string getserverdesc()
{
	return ServerDesc;
}

std::string getnetworkname()
{
	return Network;
}

std::string getadminname()
{
	return AdminName;
}

std::string getadminemail()
{
	return AdminEmail;
}

std::string getadminnick()
{
	return AdminNick;
}

void ReadConfig(bool bail, userrec* user)
{
	char dbg[MAXBUF],pauseval[MAXBUF],Value[MAXBUF],timeout[MAXBUF],NB[MAXBUF],flood[MAXBUF],MW[MAXBUF],MCON[MAXBUF];
	char AH[MAXBUF],AP[MAXBUF],AF[MAXBUF],DNT[MAXBUF],pfreq[MAXBUF],thold[MAXBUF],sqmax[MAXBUF],rqmax[MAXBUF],SLIMT[MAXBUF];
	ConnectClass c;
	std::stringstream errstr;
	include_stack.clear();
	
	if (!LoadConf(CONFIG_FILE,&config_f,&errstr))
	{
		errstr.seekg(0);
		log(DEFAULT,"There were errors in your configuration:\n%s",errstr.str().c_str());
		if (bail)
		{
			printf("There were errors in your configuration:\n%s",errstr.str().c_str());
			Exit(0);
		}
		else
		{
			char dataline[1024];
			if (user)
			{
				WriteServ(user->fd,"NOTICE %s :There were errors in the configuration file:",user->nick);
				while (!errstr.eof())
				{
					errstr.getline(dataline,1024);
					WriteServ(user->fd,"NOTICE %s :%s",user->nick,dataline);
				}
			}
			else
			{
				WriteOpers("There were errors in the configuration file:");
				while (!errstr.eof())
				{
					errstr.getline(dataline,1024);
					WriteOpers(dataline);
				}
			}
			return;
		}
	}
	  
	ConfValue("server","name",0,ServerName,&config_f);
	ConfValue("server","description",0,ServerDesc,&config_f);
	ConfValue("server","network",0,Network,&config_f);
	ConfValue("admin","name",0,AdminName,&config_f);
	ConfValue("admin","email",0,AdminEmail,&config_f);
	ConfValue("admin","nick",0,AdminNick,&config_f);
	ConfValue("files","motd",0,motd,&config_f);
	ConfValue("files","rules",0,rules,&config_f);
	ConfValue("power","diepass",0,diepass,&config_f);
	ConfValue("power","pause",0,pauseval,&config_f);
	ConfValue("power","restartpass",0,restartpass,&config_f);
	ConfValue("options","prefixquit",0,PrefixQuit,&config_f);
	ConfValue("die","value",0,DieValue,&config_f);
	ConfValue("options","loglevel",0,dbg,&config_f);
	ConfValue("options","netbuffersize",0,NB,&config_f);
	ConfValue("options","maxwho",0,MW,&config_f);
	ConfValue("options","allowhalfop",0,AH,&config_f);
	ConfValue("options","allowprotect",0,AP,&config_f);
	ConfValue("options","allowfounder",0,AF,&config_f);
	ConfValue("dns","server",0,DNSServer,&config_f);
	ConfValue("dns","timeout",0,DNT,&config_f);
	ConfValue("options","moduledir",0,ModPath,&config_f);
        ConfValue("disabled","commands",0,DisabledCommands,&config_f);
	ConfValue("options","somaxconn",0,MCON,&config_f);
	ConfValue("options","softlimit",0,SLIMT,&config_f);

	SoftLimit = atoi(SLIMT);
	if ((SoftLimit < 1) || (SoftLimit > MAXCLIENTS))
	{
		log(DEFAULT,"WARNING: <options:softlimit> value is greater than %d or less than 0, set to %d.",MAXCLIENTS,MAXCLIENTS);
		SoftLimit = MAXCLIENTS;
	}
	MaxConn = atoi(MCON);
	if (MaxConn > SOMAXCONN)
		log(DEFAULT,"WARNING: <options:somaxconn> value may be higher than the system-defined SOMAXCONN value!");
	NetBufferSize = atoi(NB);
	MaxWhoResults = atoi(MW);
	dns_timeout = atoi(DNT);
	if (!dns_timeout)
		dns_timeout = 5;
	if (!MaxConn)
		MaxConn = SOMAXCONN;
	if (!DNSServer[0])
		strlcpy(DNSServer,"127.0.0.1",MAXBUF);
	if (!ModPath[0])
		strlcpy(ModPath,MOD_PATH,MAXBUF);
	AllowHalfop = ((!strcasecmp(AH,"true")) || (!strcasecmp(AH,"1")) || (!strcasecmp(AH,"yes")));
	AllowProtect = ((!strcasecmp(AP,"true")) || (!strcasecmp(AP,"1")) || (!strcasecmp(AP,"yes")));
	AllowFounder = ((!strcasecmp(AF,"true")) || (!strcasecmp(AF,"1")) || (!strcasecmp(AF,"yes")));
	if ((!NetBufferSize) || (NetBufferSize > 65535) || (NetBufferSize < 1024))
	{
		log(DEFAULT,"No NetBufferSize specified or size out of range, setting to default of 10240.");
		NetBufferSize = 10240;
	}
	if ((!MaxWhoResults) || (MaxWhoResults > 65535) || (MaxWhoResults < 1))
	{
		log(DEFAULT,"No MaxWhoResults specified or size out of range, setting to default of 128.");
		MaxWhoResults = 128;
	}
	if (!strcmp(dbg,"debug"))
		LogLevel = DEBUG;
	if (!strcmp(dbg,"verbose"))
		LogLevel = VERBOSE;
	if (!strcmp(dbg,"default"))
		LogLevel = DEFAULT;
	if (!strcmp(dbg,"sparse"))
		LogLevel = SPARSE;
	if (!strcmp(dbg,"none"))
		LogLevel = NONE;
	readfile(MOTD,motd);
	log(DEFAULT,"Reading message of the day...");
	readfile(RULES,rules);
	log(DEFAULT,"Reading connect classes...");
	Classes.clear();
	for (int i = 0; i < ConfValueEnum("connect",&config_f); i++)
	{
		strcpy(Value,"");
		ConfValue("connect","allow",i,Value,&config_f);
		ConfValue("connect","timeout",i,timeout,&config_f);
		ConfValue("connect","flood",i,flood,&config_f);
		ConfValue("connect","pingfreq",i,pfreq,&config_f);
		ConfValue("connect","threshold",i,thold,&config_f);
		ConfValue("connect","sendq",i,sqmax,&config_f);
		ConfValue("connect","recvq",i,rqmax,&config_f);
		if (Value[0])
		{
			strlcpy(c.host,Value,MAXBUF);
			c.type = CC_ALLOW;
			strlcpy(Value,"",MAXBUF);
			ConfValue("connect","password",i,Value,&config_f);
			strlcpy(c.pass,Value,MAXBUF);
			c.registration_timeout = 90; // default is 2 minutes
			c.pingtime = 120;
			c.flood = atoi(flood);
			c.threshold = 5;
			c.sendqmax = 262144; // 256k
			c.recvqmax = 4096;   // 4k
			if (atoi(thold)>0)
			{
				c.threshold = atoi(thold);
			}
			if (atoi(sqmax)>0)
			{
				c.sendqmax = atoi(sqmax);
			}
			if (atoi(rqmax)>0)
			{
				c.recvqmax = atoi(rqmax);
			}
			if (atoi(timeout)>0)
			{
				c.registration_timeout = atoi(timeout);
			}
			if (atoi(pfreq)>0)
			{
				c.pingtime = atoi(pfreq);
			}
			Classes.push_back(c);
			log(DEBUG,"Read connect class type ALLOW, host=%s password=%s timeout=%lu flood=%lu",c.host,c.pass,(unsigned long)c.registration_timeout,(unsigned long)c.flood);
		}
		else
		{
			ConfValue("connect","deny",i,Value,&config_f);
			strlcpy(c.host,Value,MAXBUF);
			c.type = CC_DENY;
			Classes.push_back(c);
			log(DEBUG,"Read connect class type DENY, host=%s",c.host);
		}
	
	}
	log(DEFAULT,"Reading K lines,Q lines and Z lines from config...");
	read_xline_defaults();
	log(DEFAULT,"Applying K lines, Q lines and Z lines...");
	apply_lines();

	autoconns.clear();
        for (int i = 0; i < ConfValueEnum("link",&config_f); i++)
        {
		char Link_ServerName[MAXBUF],Link_AConn[MAXBUF];
                ConfValue("link","name",i,Link_ServerName,&config_f);
                ConfValue("link","autoconnect",i,Link_AConn,&config_f);
		if (strcmp(Link_AConn,""))
		{
			autoconns[std::string(Link_ServerName)] = atoi(Link_AConn) + time(NULL);
		}
        }


	log(DEFAULT,"Done reading configuration file, InspIRCd is now starting.");
	if (!bail)
	{
		log(DEFAULT,"Adding and removing modules due to rehash...");

		std::vector<std::string> old_module_names, new_module_names, added_modules, removed_modules;

		// store the old module names
		for (std::vector<std::string>::iterator t = module_names.begin(); t != module_names.end(); t++)
		{
			old_module_names.push_back(*t);
		}

		// get the new module names
		for (int count2 = 0; count2 < ConfValueEnum("module",&config_f); count2++)
		{
			ConfValue("module","name",count2,Value,&config_f);
			new_module_names.push_back(Value);
		}

		// now create a list of new modules that are due to be loaded
		// and a seperate list of modules which are due to be unloaded
		for (std::vector<std::string>::iterator _new = new_module_names.begin(); _new != new_module_names.end(); _new++)
		{
			bool added = true;
			for (std::vector<std::string>::iterator old = old_module_names.begin(); old != old_module_names.end(); old++)
			{
				if (*old == *_new)
					added = false;
			}
			if (added)
				added_modules.push_back(*_new);
		}
		for (std::vector<std::string>::iterator oldm = old_module_names.begin(); oldm != old_module_names.end(); oldm++)
		{
			bool removed = true;
			for (std::vector<std::string>::iterator newm = new_module_names.begin(); newm != new_module_names.end(); newm++)
			{
				if (*newm == *oldm)
					removed = false;
			}
			if (removed)
				removed_modules.push_back(*oldm);
		}
		// now we have added_modules, a vector of modules to be loaded, and removed_modules, a vector of modules
		// to be removed.
		int rem = 0, add = 0;
		if (!removed_modules.empty())
		for (std::vector<std::string>::iterator removing = removed_modules.begin(); removing != removed_modules.end(); removing++)
		{
			if (UnloadModule(removing->c_str()))
			{
				WriteOpers("*** REHASH UNLOADED MODULE: %s",removing->c_str());
				WriteServ(user->fd,"973 %s %s :Module %s successfully unloaded.",user->nick, removing->c_str(), removing->c_str());
				rem++;
			}
			else
			{
				WriteServ(user->fd,"972 %s %s :Failed to unload module %s: %s",user->nick, removing->c_str(), removing->c_str(), ModuleError());
			}
		}
		if (!added_modules.empty())
		for (std::vector<std::string>::iterator adding = added_modules.begin(); adding != added_modules.end(); adding++)
		{
			if (LoadModule(adding->c_str()))
			{
				WriteOpers("*** REHASH LOADED MODULE: %s",adding->c_str());
				WriteServ(user->fd,"975 %s %s :Module %s successfully loaded.",user->nick, adding->c_str(), adding->c_str());
				add++;
			}
			else
			{
				WriteServ(user->fd,"974 %s %s :Failed to load module %s: %s",user->nick, adding->c_str(), adding->c_str(), ModuleError());
			}
		}
		log(DEFAULT,"Successfully unloaded %lu of %lu modules and loaded %lu of %lu modules.",(unsigned long)rem,(unsigned long)removed_modules.size(),(unsigned long)add,(unsigned long)added_modules.size());
	}
}


/* add a channel to a user, creating the record for it if needed and linking
 * it to the user record */

chanrec* add_channel(userrec *user, const char* cn, const char* key, bool override)
{
	if ((!user) || (!cn))
	{
		log(DEFAULT,"*** BUG *** add_channel was given an invalid parameter");
		return 0;
	}

	chanrec* Ptr;
	int created = 0;
	char cname[MAXBUF];

	strncpy(cname,cn,MAXBUF);
	
	// we MUST declare this wherever we use FOREACH_RESULT
	int MOD_RESULT = 0;

	if (strlen(cname) > CHANMAX)
	{
		cname[CHANMAX] = '\0';
	}

	log(DEBUG,"add_channel: %s %s",user->nick,cname);
	
	if ((FindChan(cname)) && (has_channel(user,FindChan(cname))))
	{
		return NULL; // already on the channel!
	}


	if (!FindChan(cname))
	{
		if (!strcasecmp(ServerName,user->server))
		{
			MOD_RESULT = 0;
			FOREACH_RESULT(OnUserPreJoin(user,NULL,cname));
			if (MOD_RESULT == 1) {
				return NULL;
			}
		}

		/* create a new one */
		log(DEBUG,"add_channel: creating: %s",cname);
		{
			chanlist[cname] = new chanrec();

			strlcpy(chanlist[cname]->name, cname,CHANMAX);
			chanlist[cname]->binarymodes = CM_TOPICLOCK | CM_NOEXTERNAL;
			chanlist[cname]->created = TIME;
			strcpy(chanlist[cname]->topic, "");
			strncpy(chanlist[cname]->setby, user->nick,NICKMAX);
			chanlist[cname]->topicset = 0;
			Ptr = chanlist[cname];
			log(DEBUG,"add_channel: created: %s",cname);
			/* set created to 2 to indicate user
			 * is the first in the channel
			 * and should be given ops */
			created = 2;
		}
	}
	else
	{
		/* channel exists, just fish out a pointer to its struct */
		Ptr = FindChan(cname);
		if (Ptr)
		{
			log(DEBUG,"add_channel: joining to: %s",Ptr->name);
			
			// remote users are allowed us to bypass channel modes
			// and bans (used by servers)
			if (!strcasecmp(ServerName,user->server))
			{
				log(DEBUG,"Not overriding...");
				MOD_RESULT = 0;
				FOREACH_RESULT(OnUserPreJoin(user,Ptr,cname));
				if (MOD_RESULT == 1) {
					return NULL;
				}
				log(DEBUG,"MOD_RESULT=%d",MOD_RESULT);
				
				if (!MOD_RESULT) 
				{
					log(DEBUG,"add_channel: checking key, invite, etc");
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckKey(user, Ptr, key ? key : ""));
					if (MOD_RESULT == 0)
					{
						if (Ptr->key[0])
						{
							log(DEBUG,"add_channel: %s has key %s",Ptr->name,Ptr->key);
							if (!key)
							{
								log(DEBUG,"add_channel: no key given in JOIN");
								WriteServ(user->fd,"475 %s %s :Cannot join channel (Requires key)",user->nick, Ptr->name);
								return NULL;
							}
							else
							{
								if (strcasecmp(key,Ptr->key))
								{
									log(DEBUG,"add_channel: bad key given in JOIN");
									WriteServ(user->fd,"475 %s %s :Cannot join channel (Incorrect key)",user->nick, Ptr->name);
									return NULL;
								}
							}
						}
						log(DEBUG,"add_channel: no key");
					}
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckInvite(user, Ptr));
					if (MOD_RESULT == 0)
					{
						if (Ptr->binarymodes & CM_INVITEONLY)
						{
							log(DEBUG,"add_channel: channel is +i");
							if (user->IsInvited(Ptr->name))
							{
								/* user was invited to channel */
								/* there may be an optional channel NOTICE here */
							}
							else
							{
								WriteServ(user->fd,"473 %s %s :Cannot join channel (Invite only)",user->nick, Ptr->name);
								return NULL;
							}
						}
						log(DEBUG,"add_channel: channel is not +i");
					}
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckLimit(user, Ptr));
					if (MOD_RESULT == 0)
					{
						if (Ptr->limit)
						{
							if (usercount(Ptr) >= Ptr->limit)
							{
								WriteServ(user->fd,"471 %s %s :Cannot join channel (Channel is full)",user->nick, Ptr->name);
								return NULL;
							}
						}
					}
					log(DEBUG,"add_channel: about to walk banlist");
					MOD_RESULT = 0;
					FOREACH_RESULT(OnCheckBan(user, Ptr));
					if (MOD_RESULT == 0)
					{
						/* check user against the channel banlist */
						if (Ptr)
						{
							if (Ptr->bans.size())
							{
								for (BanList::iterator i = Ptr->bans.begin(); i != Ptr->bans.end(); i++)
								{
									if (match(user->GetFullHost(),i->data))
									{
										WriteServ(user->fd,"474 %s %s :Cannot join channel (You're banned)",user->nick, Ptr->name);
										return NULL;
									}
								}
							}
						}
						log(DEBUG,"add_channel: bans checked");
					}
				
				}
				

				if ((Ptr) && (user))
				{
					user->RemoveInvite(Ptr->name);
				}
	
				log(DEBUG,"add_channel: invites removed");

			}
			else
			{
				log(DEBUG,"Overridden checks");
			}

			
		}
		created = 1;
	}

	log(DEBUG,"Passed channel checks");
	
	for (int index =0; index != MAXCHANS; index++)
	{
		log(DEBUG,"Check location %d",index);
		if (user->chans[index].channel == NULL)
		{
			log(DEBUG,"Adding into their channel list at location %d",index);

			if (created == 2) 
			{
				/* first user in is given ops */
				user->chans[index].uc_modes = UCMODE_OP;
			}
			else
			{
				user->chans[index].uc_modes = 0;
			}
			user->chans[index].channel = Ptr;
			Ptr->AddUser((char*)user);
			WriteChannel(Ptr,user,"JOIN :%s",Ptr->name);
			
			log(DEBUG,"Sent JOIN to client");

			if (Ptr->topicset)
			{
				WriteServ(user->fd,"332 %s %s :%s", user->nick, Ptr->name, Ptr->topic);
				WriteServ(user->fd,"333 %s %s %s %lu", user->nick, Ptr->name, Ptr->setby, (unsigned long)Ptr->topicset);
			}
			userlist(user,Ptr);
			WriteServ(user->fd,"366 %s %s :End of /NAMES list.", user->nick, Ptr->name);
			//WriteServ(user->fd,"324 %s %s +%s",user->nick, Ptr->name,chanmodes(Ptr));
			//WriteServ(user->fd,"329 %s %s %lu", user->nick, Ptr->name, (unsigned long)Ptr->created);
			FOREACH_MOD OnUserJoin(user,Ptr);
			return Ptr;
		}
	}
	log(DEBUG,"add_channel: user channel max exceeded: %s %s",user->nick,cname);
	WriteServ(user->fd,"405 %s %s :You are on too many channels",user->nick, cname);
	return NULL;
}

/* remove a channel from a users record, and remove the record from memory
 * if the channel has become empty */

chanrec* del_channel(userrec *user, const char* cname, const char* reason, bool local)
{
	if ((!user) || (!cname))
	{
		log(DEFAULT,"*** BUG *** del_channel was given an invalid parameter");
		return NULL;
	}

	chanrec* Ptr;

	if ((!cname) || (!user))
	{
		return NULL;
	}

	Ptr = FindChan(cname);
	
	if (!Ptr)
	{
		return NULL;
	}

	FOREACH_MOD OnUserPart(user,Ptr);
	log(DEBUG,"del_channel: removing: %s %s",user->nick,Ptr->name);
	
	for (int i =0; i != MAXCHANS; i++)
	{
		/* zap it from the channel list of the user */
		if (user->chans[i].channel == Ptr)
		{
			if (reason)
			{
				WriteChannel(Ptr,user,"PART %s :%s",Ptr->name, reason);
			}
			else
			{
				WriteChannel(Ptr,user,"PART :%s",Ptr->name);
			}
			user->chans[i].uc_modes = 0;
			user->chans[i].channel = NULL;
			log(DEBUG,"del_channel: unlinked: %s %s",user->nick,Ptr->name);
			break;
		}
	}

	Ptr->DelUser((char*)user);
	
	/* if there are no users left on the channel */
	if (!usercount(Ptr))
	{
		chan_hash::iterator iter = chanlist.find(Ptr->name);

		log(DEBUG,"del_channel: destroying channel: %s",Ptr->name);

		/* kill the record */
		if (iter != chanlist.end())
		{
			log(DEBUG,"del_channel: destroyed: %s",Ptr->name);
			delete Ptr;
			chanlist.erase(iter);
		}
	}

	return NULL;
}


void kick_channel(userrec *src,userrec *user, chanrec *Ptr, char* reason)
{
	if ((!src) || (!user) || (!Ptr) || (!reason))
	{
		log(DEFAULT,"*** BUG *** kick_channel was given an invalid parameter");
		return;
	}

	if ((!Ptr) || (!user) || (!src))
	{
		return;
	}

	log(DEBUG,"kick_channel: removing: %s %s %s",user->nick,Ptr->name,src->nick);

	if (!has_channel(user,Ptr))
	{
		WriteServ(src->fd,"441 %s %s %s :They are not on that channel",src->nick, user->nick, Ptr->name);
		return;
	}

	int MOD_RESULT = 0;
	FOREACH_RESULT(OnAccessCheck(src,user,Ptr,AC_KICK));
	if ((MOD_RESULT == ACR_DENY) && (!is_uline(src->server)))
		return;

	if ((MOD_RESULT == ACR_DEFAULT) || (!is_uline(src->server)))
	{
 		if ((cstatus(src,Ptr) < STATUS_HOP) || (cstatus(src,Ptr) < cstatus(user,Ptr)))
		{
			if (cstatus(src,Ptr) == STATUS_HOP)
			{
				WriteServ(src->fd,"482 %s %s :You must be a channel operator",src->nick, Ptr->name);
			}
			else
			{
				WriteServ(src->fd,"482 %s %s :You must be at least a half-operator to change modes on this channel",src->nick, Ptr->name);
			}
			
			return;
		}
	}

	if (!is_uline(src->server))
	{
		MOD_RESULT = 0;
		FOREACH_RESULT(OnUserPreKick(src,user,Ptr,reason));
		if (MOD_RESULT)
			return;
	}

	FOREACH_MOD OnUserKick(src,user,Ptr,reason);

	for (int i =0; i != MAXCHANS; i++)
	{
		/* zap it from the channel list of the user */
		if (user->chans[i].channel)
		if (!strcasecmp(user->chans[i].channel->name,Ptr->name))
		{
			WriteChannel(Ptr,src,"KICK %s %s :%s",Ptr->name, user->nick, reason);
			user->chans[i].uc_modes = 0;
			user->chans[i].channel = NULL;
			log(DEBUG,"del_channel: unlinked: %s %s",user->nick,Ptr->name);
			break;
		}
	}

	Ptr->DelUser((char*)user);

	/* if there are no users left on the channel */
	if (!usercount(Ptr))
	{
		chan_hash::iterator iter = chanlist.find(Ptr->name);

		log(DEBUG,"del_channel: destroying channel: %s",Ptr->name);

		/* kill the record */
		if (iter != chanlist.end())
		{
			log(DEBUG,"del_channel: destroyed: %s",Ptr->name);
			delete Ptr;
			chanlist.erase(iter);
		}
	}
}




/* This function pokes and hacks at a parameter list like the following:
 *
 * PART #winbot,#darkgalaxy :m00!
 *
 * to turn it into a series of individual calls like this:
 *
 * PART #winbot :m00!
 * PART #darkgalaxy :m00!
 *
 * The seperate calls are sent to a callback function provided by the caller
 * (the caller will usually call itself recursively). The callback function
 * must be a command handler. Calling this function on a line with no list causes
 * no action to be taken. You must provide a starting and ending parameter number
 * where the range of the list can be found, useful if you have a terminating
 * parameter as above which is actually not part of the list, or parameters
 * before the actual list as well. This code is used by many functions which
 * can function as "one to list" (see the RFC) */

int loop_call(handlerfunc fn, char **parameters, int pcnt, userrec *u, int start, int end, int joins)
{
	char plist[MAXBUF];
	char *param;
	char *pars[32];
	char blog[32][MAXBUF];
	char blog2[32][MAXBUF];
	int j = 0, q = 0, total = 0, t = 0, t2 = 0, total2 = 0;
	char keystr[MAXBUF];
	char moo[MAXBUF];

	for (int i = 0; i <32; i++)
		strcpy(blog[i],"");

	for (int i = 0; i <32; i++)
		strcpy(blog2[i],"");

	strcpy(moo,"");
	for (int i = 0; i <10; i++)
	{
		if (!parameters[i])
		{
			parameters[i] = moo;
		}
	}
	if (joins)
	{
		if (pcnt > 1) /* we have a key to copy */
		{
			strlcpy(keystr,parameters[1],MAXBUF);
		}
	}

	if (!parameters[start])
	{
		return 0;
	}
	if (!strchr(parameters[start],','))
	{
		return 0;
	}
	strcpy(plist,"");
	for (int i = start; i <= end; i++)
	{
		if (parameters[i])
		{
			strlcat(plist,parameters[i],MAXBUF);
		}
	}
	
	j = 0;
	param = plist;

	t = strlen(plist);
	for (int i = 0; i < t; i++)
	{
		if (plist[i] == ',')
		{
			plist[i] = '\0';
			strlcpy(blog[j++],param,MAXBUF);
			param = plist+i+1;
			if (j>20)
			{
				WriteServ(u->fd,"407 %s %s :Too many targets in list, message not delivered.",u->nick,blog[j-1]);
				return 1;
			}
		}
	}
	strlcpy(blog[j++],param,MAXBUF);
	total = j;

	if ((joins) && (keystr) && (total>0)) // more than one channel and is joining
	{
		strcat(keystr,",");
	}
	
	if ((joins) && (keystr))
	{
		if (strchr(keystr,','))
		{
			j = 0;
			param = keystr;
			t2 = strlen(keystr);
			for (int i = 0; i < t2; i++)
			{
				if (keystr[i] == ',')
				{
					keystr[i] = '\0';
					strlcpy(blog2[j++],param,MAXBUF);
					param = keystr+i+1;
				}
			}
			strlcpy(blog2[j++],param,MAXBUF);
			total2 = j;
		}
	}

	for (j = 0; j < total; j++)
	{
		if (blog[j])
		{
			pars[0] = blog[j];
		}
		for (q = end; q < pcnt-1; q++)
		{
			if (parameters[q+1])
			{
				pars[q-end+1] = parameters[q+1];
			}
		}
		if ((joins) && (parameters[1]))
		{
			if (pcnt > 1)
			{
				pars[1] = blog2[j];
			}
			else
			{
				pars[1] = NULL;
			}
		}
		/* repeatedly call the function with the hacked parameter list */
		if ((joins) && (pcnt > 1))
		{
			if (pars[1])
			{
				// pars[1] already set up and containing key from blog2[j]
				fn(pars,2,u);
			}
			else
			{
				pars[1] = parameters[1];
				fn(pars,2,u);
			}
		}
		else
		{
			fn(pars,pcnt-(end-start),u);
		}
	}

	return 1;
}



void kill_link(userrec *user,const char* r)
{
	user_hash::iterator iter = clientlist.find(user->nick);
	
	char reason[MAXBUF];
	
	strncpy(reason,r,MAXBUF);

	if (strlen(reason)>MAXQUIT)
	{
		reason[MAXQUIT-1] = '\0';
	}

	log(DEBUG,"kill_link: %s '%s'",user->nick,reason);
	Write(user->fd,"ERROR :Closing link (%s@%s) [%s]",user->ident,user->host,reason);
	log(DEBUG,"closing fd %lu",(unsigned long)user->fd);

	if (user->registered == 7) {
		FOREACH_MOD OnUserQuit(user,reason);
		WriteCommonExcept(user,"QUIT :%s",reason);
	}

	user->FlushWriteBuf();

	FOREACH_MOD OnUserDisconnect(user);

	if (user->fd > -1)
	{
		FOREACH_MOD OnRawSocketClose(user->fd);
		SE->DelFd(user->fd);
		user->CloseSocket();
	}

	// this must come before the WriteOpers so that it doesnt try to fill their buffer with anything
	// if they were an oper with +s.
        if (user->registered == 7) {
                purge_empty_chans(user);
		// fix by brain: only show local quits because we only show local connects (it just makes SENSE)
		if (!strcmp(user->server,ServerName))
			WriteOpers("*** Client exiting: %s!%s@%s [%s]",user->nick,user->ident,user->host,reason);
		AddWhoWas(user);
	}

	if (iter != clientlist.end())
	{
		log(DEBUG,"deleting user hash value %lu",(unsigned long)user);
		if (user->fd > -1)
			fd_ref_table[user->fd] = NULL;
		clientlist.erase(iter);
	}
	delete user;
}

void kill_link_silent(userrec *user,const char* r)
{
	user_hash::iterator iter = clientlist.find(user->nick);
	
	char reason[MAXBUF];
	
	strncpy(reason,r,MAXBUF);

	if (strlen(reason)>MAXQUIT)
	{
		reason[MAXQUIT-1] = '\0';
	}

	log(DEBUG,"kill_link: %s '%s'",user->nick,reason);
	Write(user->fd,"ERROR :Closing link (%s@%s) [%s]",user->ident,user->host,reason);
	log(DEBUG,"closing fd %lu",(unsigned long)user->fd);

	user->FlushWriteBuf();

	if (user->registered == 7) {
		FOREACH_MOD OnUserQuit(user,reason);
		WriteCommonExcept(user,"QUIT :%s",reason);
	}

	FOREACH_MOD OnUserDisconnect(user);

        if (user->fd > -1)
        {
		FOREACH_MOD OnRawSocketClose(user->fd);
		SE->DelFd(user->fd);
		user->CloseSocket();
        }

        if (user->registered == 7) {
                purge_empty_chans(user);
        }
	
	if (iter != clientlist.end())
	{
		log(DEBUG,"deleting user hash value %lu",(unsigned long)user);
                if (user->fd > -1)
                        fd_ref_table[user->fd] = NULL;
		clientlist.erase(iter);
	}
	delete user;
}


/*void *task(void *arg)
{
	for (;;) { 
		cout << (char *)arg;
		cout.flush();
	}
	return NULL;
}*/

int main(int argc, char** argv)
{
	Start();
	srand(time(NULL));
	log(DEBUG,"*** InspIRCd starting up!");
	if (!FileExists(CONFIG_FILE))
	{
		printf("ERROR: Cannot open config file: %s\nExiting...\n",CONFIG_FILE);
		log(DEFAULT,"main: no config");
		printf("ERROR: Your config file is missing, this IRCd will self destruct in 10 seconds!\n");
		Exit(ERROR);
	}
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
		{
			if (!strcmp(argv[i],"-nofork")) {
				nofork = true;
			}
			if (!strcmp(argv[i],"-wait")) {
				sleep(6);
			}
			if (!strcmp(argv[i],"-nolimit")) {
				unlimitcore = true;
			}
		}
	}

	strlcpy(MyExecutable,argv[0],MAXBUF);
	
	// initialize the lowercase mapping table
	for (unsigned int cn = 0; cn < 256; cn++)
		lowermap[cn] = cn;
	// lowercase the uppercase chars
	for (unsigned int cn = 65; cn < 91; cn++)
		lowermap[cn] = tolower(cn);
	// now replace the specific chars for scandanavian comparison
	lowermap[(unsigned)'['] = '{';
	lowermap[(unsigned)']'] = '}';
	lowermap[(unsigned)'\\'] = '|';

	if (InspIRCd(argv,argc) == ERROR)
	{
		log(DEFAULT,"main: daemon function bailed");
		printf("ERROR: could not initialise. Shutting down.\n");
		Exit(ERROR);
	}
	Exit(TRUE);
	return 0;
}

template<typename T> inline string ConvToStr(const T &in)
{
	stringstream tmp;
	if (!(tmp << in)) return string();
	return tmp.str();
}

/* re-allocates a nick in the user_hash after they change nicknames,
 * returns a pointer to the new user as it may have moved */

userrec* ReHashNick(char* Old, char* New)
{
	//user_hash::iterator newnick;
	user_hash::iterator oldnick = clientlist.find(Old);

	log(DEBUG,"ReHashNick: %s %s",Old,New);
	
	if (!strcasecmp(Old,New))
	{
		log(DEBUG,"old nick is new nick, skipping");
		return oldnick->second;
	}
	
	if (oldnick == clientlist.end()) return NULL; /* doesnt exist */

	log(DEBUG,"ReHashNick: Found hashed nick %s",Old);

	userrec* olduser = oldnick->second;
	clientlist[New] = olduser;
	clientlist.erase(oldnick);

	log(DEBUG,"ReHashNick: Nick rehashed as %s",New);
	
	return clientlist[New];
}

/* adds or updates an entry in the whowas list */
void AddWhoWas(userrec* u)
{
	whowas_hash::iterator iter = whowas.find(u->nick);
	WhoWasUser *a = new WhoWasUser();
	strlcpy(a->nick,u->nick,NICKMAX);
	strlcpy(a->ident,u->ident,IDENTMAX);
	strlcpy(a->dhost,u->dhost,160);
	strlcpy(a->host,u->host,160);
	strlcpy(a->fullname,u->fullname,MAXGECOS);
	strlcpy(a->server,u->server,256);
	a->signon = u->signon;

	/* MAX_WHOWAS:   max number of /WHOWAS items
	 * WHOWAS_STALE: number of hours before a WHOWAS item is marked as stale and
	 *		 can be replaced by a newer one
	 */
	
	if (iter == whowas.end())
	{
		if (whowas.size() >= (unsigned)WHOWAS_MAX)
		{
			for (whowas_hash::iterator i = whowas.begin(); i != whowas.end(); i++)
			{
				// 3600 seconds in an hour ;)
				if ((i->second->signon)<(TIME-(WHOWAS_STALE*3600)))
				{
					// delete the old one
					if (i->second) delete i->second;
					// replace with new one
					i->second = a;
					log(DEBUG,"added WHOWAS entry, purged an old record");
					return;
				}
			}
			// no space left and user doesnt exist. Don't leave ram in use!
			log(DEBUG,"Not able to update whowas (list at WHOWAS_MAX entries and trying to add new?), freeing excess ram");
			delete a;
		}
		else
		{
			log(DEBUG,"added fresh WHOWAS entry");
			whowas[a->nick] = a;
		}
	}
	else
	{
		log(DEBUG,"updated WHOWAS entry");
		if (iter->second) delete iter->second;
		iter->second = a;
	}
}

#ifdef THREADED_DNS
void* dns_task(void* arg)
{
	userrec* u = (userrec*)arg;
	log(DEBUG,"DNS thread for user %s",u->nick);
	DNS dns1;
	DNS dns2;
	std::string host;
	std::string ip;
	if (dns1.ReverseLookup(u->ip))
	{
		log(DEBUG,"DNS Step 1");
		while (!dns1.HasResult())
		{
			usleep(100);
		}
		host = dns1.GetResult();
		if (host != "")
		{
			log(DEBUG,"DNS Step 2: '%s'",host.c_str());
			if (dns2.ForwardLookup(host))
			{
				while (!dns2.HasResult())
				{
					usleep(100);
				}
				ip = dns2.GetResultIP();
				log(DEBUG,"DNS Step 3 '%s'(%d) '%s'(%d)",ip.c_str(),ip.length(),u->ip,strlen(u->ip));
				if (ip == std::string(u->ip))
				{
					log(DEBUG,"DNS Step 4");
					if (host.length() < 160)
					{
						log(DEBUG,"DNS Step 5");
						strcpy(u->host,host.c_str());
						strcpy(u->dhost,host.c_str());
					}
				}
			}
		}
	}
	u->dns_done = true;
	return NULL;
}
#endif

/* add a client connection to the sockets list */
void AddClient(int socket, char* host, int port, bool iscached, char* ip)
{
	string tempnick;
	char tn2[MAXBUF];
	user_hash::iterator iter;

	tempnick = ConvToStr(socket) + "-unknown";
	sprintf(tn2,"%lu-unknown",(unsigned long)socket);

	iter = clientlist.find(tempnick);

	// fix by brain.
	// as these nicknames are 'RFC impossible', we can be sure nobody is going to be
	// using one as a registered connection. As theyre per fd, we can also safely assume
	// that we wont have collisions. Therefore, if the nick exists in the list, its only
	// used by a dead socket, erase the iterator so that the new client may reclaim it.
	// this was probably the cause of 'server ignores me when i hammer it with reconnects'
	// issue in earlier alphas/betas
	if (iter != clientlist.end())
	{
		userrec* goner = iter->second;
		delete goner;
		clientlist.erase(iter);
	}

	/*
	 * It is OK to access the value here this way since we know
	 * it exists, we just created it above.
	 *
	 * At NO other time should you access a value in a map or a
	 * hash_map this way.
	 */
	clientlist[tempnick] = new userrec();

	NonBlocking(socket);
	log(DEBUG,"AddClient: %lu %s %d %s",(unsigned long)socket,host,port,ip);

	clientlist[tempnick]->fd = socket;
	strlcpy(clientlist[tempnick]->nick, tn2,NICKMAX);
	strlcpy(clientlist[tempnick]->host, host,160);
	strlcpy(clientlist[tempnick]->dhost, host,160);
	clientlist[tempnick]->server = (char*)FindServerNamePtr(ServerName);
	strlcpy(clientlist[tempnick]->ident, "unknown",IDENTMAX);
	clientlist[tempnick]->registered = 0;
	clientlist[tempnick]->signon = TIME+dns_timeout;
	clientlist[tempnick]->lastping = 1;
	clientlist[tempnick]->port = port;
	strlcpy(clientlist[tempnick]->ip,ip,16);

	// set the registration timeout for this user
	unsigned long class_regtimeout = 90;
	int class_flood = 0;
	long class_threshold = 5;
	long class_sqmax = 262144;	// 256kb
	long class_rqmax = 4096;	// 4k

	for (ClassVector::iterator i = Classes.begin(); i != Classes.end(); i++)
	{
		if (match(clientlist[tempnick]->host,i->host) && (i->type == CC_ALLOW))
		{
			class_regtimeout = (unsigned long)i->registration_timeout;
			class_flood = i->flood;
			clientlist[tempnick]->pingmax = i->pingtime;
			class_threshold = i->threshold;
			class_sqmax = i->sendqmax;
			class_rqmax = i->recvqmax;
			break;
		}
	}

	clientlist[tempnick]->nping = TIME+clientlist[tempnick]->pingmax+dns_timeout;
	clientlist[tempnick]->timeout = TIME+class_regtimeout;
	clientlist[tempnick]->flood = class_flood;
	clientlist[tempnick]->threshold = class_threshold;
	clientlist[tempnick]->sendqmax = class_sqmax;
	clientlist[tempnick]->recvqmax = class_rqmax;

	for (int i = 0; i < MAXCHANS; i++)
	{
 		clientlist[tempnick]->chans[i].channel = NULL;
 		clientlist[tempnick]->chans[i].uc_modes = 0;
 	}

	if (clientlist.size() > SoftLimit)
	{
		kill_link(clientlist[tempnick],"No more connections allowed");
		return;
	}

	if (clientlist.size() >= MAXCLIENTS)
	{
		kill_link(clientlist[tempnick],"No more connections allowed");
		return;
	}

	// this is done as a safety check to keep the file descriptors within range of fd_ref_table.
	// its a pretty big but for the moment valid assumption:
	// file descriptors are handed out starting at 0, and are recycled as theyre freed.
	// therefore if there is ever an fd over 65535, 65536 clients must be connected to the
	// irc server at once (or the irc server otherwise initiating this many connections, files etc)
	// which for the time being is a physical impossibility (even the largest networks dont have more
	// than about 10,000 users on ONE server!)
	if ((unsigned)socket > 65534)
	{
		kill_link(clientlist[tempnick],"Server is full");
		return;
	}
		

        char* e = matches_exception(ip);
	if (!e)
	{
		char* r = matches_zline(ip);
		if (r)
		{
			char reason[MAXBUF];
			snprintf(reason,MAXBUF,"Z-Lined: %s",r);
			kill_link(clientlist[tempnick],reason);
			return;
		}
	}
	fd_ref_table[socket] = clientlist[tempnick];
	SE->AddFd(socket,true,X_ESTAB_CLIENT);

	// initialize their dns lookup thread
	//if (pthread_create(&clientlist[tempnick]->dnsthread, NULL, dns_task, (void *)clientlist[tempnick]) != 0)
	//{
	//	log(DEBUG,"Failed to create DNS lookup thread for user %s",clientlist[tempnick]->nick);
	//}
}

/* shows the message of the day, and any other on-logon stuff */
void FullConnectUser(userrec* user)
{
	statsConnects++;
        user->idle_lastmsg = TIME;
        log(DEBUG,"ConnectUser: %s",user->nick);

        if ((strcmp(Passwd(user),"")) && (!user->haspassed))
        {
                kill_link(user,"Invalid password");
                return;
        }
        if (IsDenied(user))
        {
                kill_link(user,"Unauthorised connection");
                return;
        }

        char match_against[MAXBUF];
        snprintf(match_against,MAXBUF,"%s@%s",user->ident,user->host);
	char* e = matches_exception(match_against);
	if (!e)
	{
        	char* r = matches_gline(match_against);
        	if (r)
        	{
                	char reason[MAXBUF];
                	snprintf(reason,MAXBUF,"G-Lined: %s",r);
                	kill_link_silent(user,reason);
                	return;
        	}
        	r = matches_kline(user->host);
        	if (r)
        	{
                	char reason[MAXBUF];
                	snprintf(reason,MAXBUF,"K-Lined: %s",r);
                	kill_link_silent(user,reason);
                	return;
	        }
	}


        WriteServ(user->fd,"NOTICE Auth :Welcome to \002%s\002!",Network);
        WriteServ(user->fd,"001 %s :Welcome to the %s IRC Network %s!%s@%s",user->nick,Network,user->nick,user->ident,user->host);
        WriteServ(user->fd,"002 %s :Your host is %s, running version %s",user->nick,ServerName,VERSION);
        WriteServ(user->fd,"003 %s :This server was created %s %s",user->nick,__TIME__,__DATE__);
        WriteServ(user->fd,"004 %s %s %s iowghraAsORVSxNCWqBzvdHtGI lvhopsmntikrRcaqOALQbSeKVfHGCuzN",user->nick,ServerName,VERSION);
        // the neatest way to construct the initial 005 numeric, considering the number of configure constants to go in it...
        std::stringstream v;
        v << "WALLCHOPS MODES=13 CHANTYPES=# PREFIX=(ohv)@%+ MAP SAFELIST MAXCHANNELS=" << MAXCHANS;
        v << " MAXBANS=60 NICKLEN=" << NICKMAX;
        v << " TOPICLEN=" << MAXTOPIC << " KICKLEN=" << MAXKICK << " MAXTARGETS=20 AWAYLEN=" << MAXAWAY << " CHANMODES=ohvb,k,l,psmnti NETWORK=";
        v << Network;
        std::string data005 = v.str();
        FOREACH_MOD On005Numeric(data005);
        // anfl @ #ratbox, efnet reminded me that according to the RFC this cant contain more than 13 tokens per line...
        // so i'd better split it :)
        std::stringstream out(data005);
        std::string token = "";
        std::string line5 = "";
        int token_counter = 0;
        while (!out.eof())
        {
                out >> token;
                line5 = line5 + token + " ";
                token_counter++;
                if ((token_counter >= 13) || (out.eof() == true))
                {
                        WriteServ(user->fd,"005 %s %s:are supported by this server",user->nick,line5.c_str());
                        line5 = "";
                        token_counter = 0;
                }
        }
        ShowMOTD(user);

	// fix 3 by brain, move registered = 7 below these so that spurious modes and host changes dont go out
	// onto the network and produce 'fake direction'
	FOREACH_MOD OnUserConnect(user);
	FOREACH_MOD OnGlobalConnect(user);
	user->registered = 7;
	WriteOpers("*** Client connecting on port %lu: %s!%s@%s [%s]",(unsigned long)user->port,user->nick,user->ident,user->host,user->ip);
}


/* shows the message of the day, and any other on-logon stuff */
void ConnectUser(userrec *user)
{
	// dns is already done, things are fast. no need to wait for dns to complete just pass them straight on
	if ((user->dns_done) && (user->registered >= 3) && (AllModulesReportReady(user)))
	{
		FullConnectUser(user);
	}
}

std::string GetVersionString()
{
        char Revision[] = "$Revision$";
	char versiondata[MAXBUF];
        char *s1 = Revision;
        char *savept;
        char *v2 = strtok_r(s1," ",&savept);
        s1 = savept;
        v2 = strtok_r(s1," ",&savept);
        s1 = savept;
#ifdef THREADED_DNS
	char dnsengine[] = "multithread";
#else
	char dnsengine[] = "singlethread";
#endif
	snprintf(versiondata,MAXBUF,"%s Rev. %s %s :%s [FLAGS=%lu,%s,%s]",VERSION,v2,ServerName,SYSTEM,(unsigned long)OPTIMISATION,SE->GetName().c_str(),dnsengine);
	return versiondata;
}

void handle_version(char **parameters, int pcnt, userrec *user)
{
	WriteServ(user->fd,"351 %s :%s",user->nick,GetVersionString().c_str());
}


bool is_valid_cmd(const char* commandname, int pcnt, userrec * user)
{
	for (unsigned int i = 0; i < cmdlist.size(); i++)
	{
		if (!strcasecmp(cmdlist[i].command,commandname))
		{
			if (cmdlist[i].handler_function)
			{
				if ((pcnt>=cmdlist[i].min_params) && (strcasecmp(cmdlist[i].source,"<core>")))
				{
					if ((strchr(user->modes,cmdlist[i].flags_needed)) || (!cmdlist[i].flags_needed))
					{
						if (cmdlist[i].flags_needed)
						{
							if ((user->HasPermission((char*)commandname)) || (is_uline(user->server)))
							{
								return true;
							}
							else
							{
								return false;
							}
						}
						return true;
					}
				}
			}
		}
	}
	return false;
}

// calls a handler function for a command

void call_handler(const char* commandname,char **parameters, int pcnt, userrec *user)
{
	for (unsigned int i = 0; i < cmdlist.size(); i++)
	{
		if (!strcasecmp(cmdlist[i].command,commandname))
		{
			if (cmdlist[i].handler_function)
			{
				if (pcnt>=cmdlist[i].min_params)
				{
					if ((strchr(user->modes,cmdlist[i].flags_needed)) || (!cmdlist[i].flags_needed))
					{
						if (cmdlist[i].flags_needed)
						{
							if ((user->HasPermission((char*)commandname)) || (is_uline(user->server)))
							{
								cmdlist[i].handler_function(parameters,pcnt,user);
							}
						}
						else
						{
							cmdlist[i].handler_function(parameters,pcnt,user);
						}
					}
				}
			}
		}
	}
}


void force_nickchange(userrec* user,const char* newnick)
{
	char nick[MAXBUF];
	int MOD_RESULT = 0;
	
	strcpy(nick,"");

	FOREACH_RESULT(OnUserPreNick(user,newnick));
	if (MOD_RESULT) {
		statsCollisions++;
		kill_link(user,"Nickname collision");
		return;
	}
	if (matches_qline(newnick))
	{
		statsCollisions++;
		kill_link(user,"Nickname collision");
		return;
	}
	
	if (user)
	{
		if (newnick)
		{
			strncpy(nick,newnick,MAXBUF);
		}
		if (user->registered == 7)
		{
			char* pars[1];
			pars[0] = nick;
			handle_nick(pars,1,user);
		}
	}
}
				

int process_parameters(char **command_p,char *parameters)
{
	int j = 0;
	int q = strlen(parameters);
	if (!q)
	{
		/* no parameters, command_p invalid! */
		return 0;
	}
	if (parameters[0] == ':')
	{
		command_p[0] = parameters+1;
		return 1;
	}
	if (q)
	{
		if ((strchr(parameters,' ')==NULL) || (parameters[0] == ':'))
		{
			/* only one parameter */
			command_p[0] = parameters;
			if (parameters[0] == ':')
			{
				if (strchr(parameters,' ') != NULL)
				{
					command_p[0]++;
				}
			}
			return 1;
		}
	}
	command_p[j++] = parameters;
	for (int i = 0; i <= q; i++)
	{
		if (parameters[i] == ' ')
		{
			command_p[j++] = parameters+i+1;
			parameters[i] = '\0';
			if (command_p[j-1][0] == ':')
			{
				*command_p[j-1]++; /* remove dodgy ":" */
				break;
				/* parameter like this marks end of the sequence */
			}
		}
	}
	return j; /* returns total number of items in the list */
}

void process_command(userrec *user, char* cmd)
{
	char *parameters;
	char *command;
	char *command_p[127];
	char p[MAXBUF], temp[MAXBUF];
	int j, items, cmd_found;

	for (int i = 0; i < 127; i++)
		command_p[i] = NULL;

	if (!user)
	{
		return;
	}
	if (!cmd)
	{
		return;
	}
	if (!cmd[0])
	{
		return;
	}
	
	int total_params = 0;
	if (strlen(cmd)>2)
	{
		for (unsigned int q = 0; q < strlen(cmd)-1; q++)
		{
			if ((cmd[q] == ' ') && (cmd[q+1] == ':'))
			{
				total_params++;
				// found a 'trailing', we dont count them after this.
				break;
			}
			if (cmd[q] == ' ')
				total_params++;
		}
	}

	// another phidjit bug...
	if (total_params > 126)
	{
		*(strchr(cmd,' ')) = '\0';
		WriteServ(user->fd,"421 %s %s :Too many parameters given",user->nick,cmd);
		return;
	}

	strlcpy(temp,cmd,MAXBUF);
	
	std::string tmp = cmd;
	for (int i = 0; i <= MODCOUNT; i++)
	{
		std::string oldtmp = tmp;
		modules[i]->OnServerRaw(tmp,true,user);
		if (oldtmp != tmp)
		{
			log(DEBUG,"A Module changed the input string!");
			log(DEBUG,"New string: %s",tmp.c_str());
			log(DEBUG,"Old string: %s",oldtmp.c_str());
			break;
		}
	}
  	strlcpy(cmd,tmp.c_str(),MAXBUF);
	strlcpy(temp,cmd,MAXBUF);

	if (!strchr(cmd,' '))
	{
		/* no parameters, lets skip the formalities and not chop up
		 * the string */
		log(DEBUG,"About to preprocess command with no params");
		items = 0;
		command_p[0] = NULL;
		parameters = NULL;
		for (unsigned int i = 0; i <= strlen(cmd); i++)
		{
			cmd[i] = toupper(cmd[i]);
		}
		command = cmd;
	}
	else
	{
		strcpy(cmd,"");
		j = 0;
		/* strip out extraneous linefeeds through mirc's crappy pasting (thanks Craig) */
		for (unsigned int i = 0; i < strlen(temp); i++)
		{
			if ((temp[i] != 10) && (temp[i] != 13) && (temp[i] != 0) && (temp[i] != 7))
			{
				cmd[j++] = temp[i];
				cmd[j] = 0;
			}
		}
		/* split the full string into a command plus parameters */
		parameters = p;
		strcpy(p," ");
		command = cmd;
		if (strchr(cmd,' '))
		{
			for (unsigned int i = 0; i <= strlen(cmd); i++)
			{
				/* capitalise the command ONLY, leave params intact */
				cmd[i] = toupper(cmd[i]);
				/* are we nearly there yet?! :P */
				if (cmd[i] == ' ')
				{
					command = cmd;
					parameters = cmd+i+1;
					cmd[i] = '\0';
					break;
				}
			}
		}
		else
		{
			for (unsigned int i = 0; i <= strlen(cmd); i++)
			{
				cmd[i] = toupper(cmd[i]);
			}
		}

	}
	cmd_found = 0;
	
	if (strlen(command)>MAXCOMMAND)
	{
		WriteServ(user->fd,"421 %s %s :Command too long",user->nick,command);
		return;
	}
	
	for (unsigned int x = 0; x < strlen(command); x++)
	{
		if (((command[x] < 'A') || (command[x] > 'Z')) && (command[x] != '.'))
		{
			if (((command[x] < '0') || (command[x]> '9')) && (command[x] != '-'))
			{
				if (strchr("@!\"$%^&*(){}[]_=+;:'#~,<>/?\\|`",command[x]))
				{
					statsUnknown++;
					WriteServ(user->fd,"421 %s %s :Unknown command",user->nick,command);
					return;
				}
			}
		}
	}

	for (unsigned int i = 0; i != cmdlist.size(); i++)
	{
		if (cmdlist[i].command[0])
		{
			if (strlen(command)>=(strlen(cmdlist[i].command))) if (!strncmp(command, cmdlist[i].command,MAXCOMMAND))
			{
				if (parameters)
				{
					if (parameters[0])
					{
						items = process_parameters(command_p,parameters);
					}
					else
					{
						items = 0;
						command_p[0] = NULL;
					}
				}
				else
				{
					items = 0;
					command_p[0] = NULL;
				}
				
				if (user)
				{
					/* activity resets the ping pending timer */
					user->nping = TIME + user->pingmax;
					if ((items) < cmdlist[i].min_params)
					{
					        log(DEBUG,"process_command: not enough parameters: %s %s",user->nick,command);
						WriteServ(user->fd,"461 %s %s :Not enough parameters",user->nick,command);
						return;
					}
					if ((!strchr(user->modes,cmdlist[i].flags_needed)) && (cmdlist[i].flags_needed))
					{
					        log(DEBUG,"process_command: permission denied: %s %s",user->nick,command);
						WriteServ(user->fd,"481 %s :Permission Denied- You do not have the required operator privilages",user->nick);
						cmd_found = 1;
						return;
					}
					if ((cmdlist[i].flags_needed) && (!user->HasPermission(command)))
					{
					        log(DEBUG,"process_command: permission denied: %s %s",user->nick,command);
						WriteServ(user->fd,"481 %s :Permission Denied- Oper type %s does not have access to command %s",user->nick,user->oper,command);
						cmd_found = 1;
						return;
					}
					/* if the command isnt USER, PASS, or NICK, and nick is empty,
					 * deny command! */
					if ((strncmp(command,"USER",4)) && (strncmp(command,"NICK",4)) && (strncmp(command,"PASS",4)))
					{
						if ((!isnick(user->nick)) || (user->registered != 7))
						{
						        log(DEBUG,"process_command: not registered: %s %s",user->nick,command);
							WriteServ(user->fd,"451 %s :You have not registered",command);
							return;
						}
					}
					if ((user->registered == 7) && (!strchr(user->modes,'o')))
					{
						std::stringstream dcmds(DisabledCommands);
						while (!dcmds.eof())
						{
							std::string thiscmd;
							dcmds >> thiscmd;
							if (!strcasecmp(thiscmd.c_str(),command))
							{
								// command is disabled!
								WriteServ(user->fd,"421 %s %s :This command has been disabled.",user->nick,command);
								return;
							}
						}
					}
					if ((user->registered == 7) || (!strncmp(command,"USER",4)) || (!strncmp(command,"NICK",4)) || (!strncmp(command,"PASS",4)))
					{
						if (cmdlist[i].handler_function)
						{
							
							/* ikky /stats counters */
							if (temp)
							{
								cmdlist[i].use_count++;
								cmdlist[i].total_bytes+=strlen(temp);
							}

							int MOD_RESULT = 0;
							FOREACH_RESULT(OnPreCommand(command,command_p,items,user));
							if (MOD_RESULT == 1) {
								return;
							}

							/* WARNING: nothing may come after the
							 * command handler call, as the handler
							 * may free the user structure! */

							cmdlist[i].handler_function(command_p,items,user);
						}
						return;
					}
					else
					{
						WriteServ(user->fd,"451 %s :You have not registered",command);
						return;
					}
				}
				cmd_found = 1;
			}
		}
	}
	if ((!cmd_found) && (user))
	{
		statsUnknown++;
		WriteServ(user->fd,"421 %s %s :Unknown command",user->nick,command);
	}
}

bool removecommands(const char* source)
{
	bool go_again = true;
	while (go_again)
	{
		go_again = false;
		for (std::deque<command_t>::iterator i = cmdlist.begin(); i != cmdlist.end(); i++)
		{
			if (!strcmp(i->source,source))
			{
				log(DEBUG,"removecommands(%s) Removing dependent command: %s",i->source,i->command);
				cmdlist.erase(i);
				go_again = true;
				break;
			}
		}
	}
	return true;
}


void process_buffer(const char* cmdbuf,userrec *user)
{
	if (!user)
	{
		log(DEFAULT,"*** BUG *** process_buffer was given an invalid parameter");
		return;
	}
	char cmd[MAXBUF];
	if (!cmdbuf)
	{
		log(DEFAULT,"*** BUG *** process_buffer was given an invalid parameter");
		return;
	}
	if (!cmdbuf[0])
	{
		return;
	}
	while (*cmdbuf == ' ') cmdbuf++; // strip leading spaces

	strlcpy(cmd,cmdbuf,MAXBUF);
	if (!cmd[0])
	{
		return;
	}
	int sl = strlen(cmd)-1;
	if ((cmd[sl] == 13) || (cmd[sl] == 10))
	{
		cmd[sl] = '\0';
	}
	sl = strlen(cmd)-1;
	if ((cmd[sl] == 13) || (cmd[sl] == 10))
	{
		cmd[sl] = '\0';
	}
	sl = strlen(cmd)-1;
	while (cmd[sl] == ' ') // strip trailing spaces
	{
		cmd[sl] = '\0';
		sl = strlen(cmd)-1;
	}

	if (!cmd[0])
	{
		return;
	}
        log(DEBUG,"CMDIN: %s %s",user->nick,cmd);
	tidystring(cmd);
	if ((user) && (cmd))
	{
		process_command(user,cmd);
	}
}

char MODERR[MAXBUF];

char* ModuleError()
{
	return MODERR;
}

void erase_factory(int j)
{
	int v = 0;
	for (std::vector<ircd_module*>::iterator t = factory.begin(); t != factory.end(); t++)
	{
		if (v == j)
		{
                	factory.erase(t);
                 	factory.push_back(NULL);
                 	return;
           	}
		v++;
     	}
}

void erase_module(int j)
{
	int v1 = 0;
	for (std::vector<Module*>::iterator m = modules.begin(); m!= modules.end(); m++)
        {
                if (v1 == j)
                {
			delete *m;
                        modules.erase(m);
                        modules.push_back(NULL);
			break;
                }
		v1++;
        }
	int v2 = 0;
        for (std::vector<std::string>::iterator v = module_names.begin(); v != module_names.end(); v++)
        {
                if (v2 == j)
                {
                       module_names.erase(v);
                       break;
                }
		v2++;
        }

}

bool UnloadModule(const char* filename)
{
	std::string filename_str = filename;
	for (unsigned int j = 0; j != module_names.size(); j++)
	{
		if (module_names[j] == filename_str)
		{
			if (modules[j]->GetVersion().Flags & VF_STATIC)
			{
				log(DEFAULT,"Failed to unload STATIC module %s",filename);
				snprintf(MODERR,MAXBUF,"Module not unloadable (marked static)");
				return false;
			}
			/* Give the module a chance to tidy out all its metadata */
			for (chan_hash::iterator c = chanlist.begin(); c != chanlist.end(); c++)
			{
				modules[j]->OnCleanup(TYPE_CHANNEL,c->second);
			}
			for (user_hash::iterator u = clientlist.begin(); u != clientlist.end(); u++)
			{
				modules[j]->OnCleanup(TYPE_USER,u->second);
			}
			FOREACH_MOD OnUnloadModule(modules[j],module_names[j]);
			// found the module
			log(DEBUG,"Deleting module...");
			erase_module(j);
			log(DEBUG,"Erasing module entry...");
			erase_factory(j);
                        log(DEBUG,"Removing dependent commands...");
                        removecommands(filename);
			log(DEFAULT,"Module %s unloaded",filename);
			MODCOUNT--;
			return true;
		}
	}
	log(DEFAULT,"Module %s is not loaded, cannot unload it!",filename);
	snprintf(MODERR,MAXBUF,"Module not loaded");
	return false;
}

bool LoadModule(const char* filename)
{
	char modfile[MAXBUF];
#ifdef STATIC_LINK
	snprintf(modfile,MAXBUF,"%s",filename);
#else
	snprintf(modfile,MAXBUF,"%s/%s",ModPath,filename);
#endif
	std::string filename_str = filename;
#ifndef STATIC_LINK
	if (!DirValid(modfile))
	{
		log(DEFAULT,"Module %s is not within the modules directory.",modfile);
		snprintf(MODERR,MAXBUF,"Module %s is not within the modules directory.",modfile);
		return false;
	}
#endif
	log(DEBUG,"Loading module: %s",modfile);
#ifndef STATIC_LINK
        if (FileExists(modfile))
        {
#endif
		for (unsigned int j = 0; j < module_names.size(); j++)
		{
			if (module_names[j] == filename_str)
			{
				log(DEFAULT,"Module %s is already loaded, cannot load a module twice!",modfile);
				snprintf(MODERR,MAXBUF,"Module already loaded");
				return false;
			}
		}
		ircd_module* a = new ircd_module(modfile);
                factory[MODCOUNT+1] = a;
                if (factory[MODCOUNT+1]->LastError())
                {
                        log(DEFAULT,"Unable to load %s: %s",modfile,factory[MODCOUNT+1]->LastError());
			snprintf(MODERR,MAXBUF,"Loader/Linker error: %s",factory[MODCOUNT+1]->LastError());
			MODCOUNT--;
			return false;
                }
                if (factory[MODCOUNT+1]->factory)
                {
			Module* m = factory[MODCOUNT+1]->factory->CreateModule(MyServer);
                        modules[MODCOUNT+1] = m;
                        /* save the module and the module's classfactory, if
                         * this isnt done, random crashes can occur :/ */
                        module_names.push_back(filename);
                }
		else
                {
                        log(DEFAULT,"Unable to load %s",modfile);
			snprintf(MODERR,MAXBUF,"Factory function failed!");
			return false;
                }
#ifndef STATIC_LINK
        }
        else
        {
                log(DEFAULT,"InspIRCd: startup: Module Not Found %s",modfile);
		snprintf(MODERR,MAXBUF,"Module file could not be found");
		return false;
        }
#endif
	MODCOUNT++;
	FOREACH_MOD OnLoadModule(modules[MODCOUNT],filename_str);
	return true;
}


void ProcessUser(userrec* cu)
{
	int result = EAGAIN;
        log(DEBUG,"Processing user with fd %d",cu->fd);
        int MOD_RESULT = 0;
        int result2 = 0;
        FOREACH_RESULT(OnRawSocketRead(cu->fd,data,65535,result2));
        if (!MOD_RESULT)
        {
		result = cu->ReadData(data, 65535);
        }
        else
        {
                log(DEBUG,"Data result returned by module: %d",MOD_RESULT);
                result = result2;
        }
        log(DEBUG,"Read result: %d",result);
        if (result)
        {
		statsRecv += result;
                // perform a check on the raw buffer as an array (not a string!) to remove
                // characters 0 and 7 which are illegal in the RFC - replace them with spaces.
                // hopefully this should stop even more people whining about "Unknown command: *"
                for (int checker = 0; checker < result; checker++)
                {
			if ((data[checker] == 0) || (data[checker] == 7))
				data[checker] = ' ';
                }
                if (result > 0)
			data[result] = '\0';
                userrec* current = cu;
                int currfd = current->fd;
                int floodlines = 0;
                // add the data to the users buffer
                if (result > 0)
		{
			if (!current->AddBuffer(data))
			{
				// AddBuffer returned false, theres too much data in the user's buffer and theyre up to no good.
				if (current->registered == 7)
                                {
                                	kill_link(current,"RecvQ exceeded");
                                }
                                else
                                {
                                        WriteOpers("*** Excess flood from %s",current->ip);
                                        log(DEFAULT,"Excess flood from: %s",current->ip);
                                        add_zline(120,ServerName,"Flood from unregistered connection",current->ip);
                                        apply_lines();
                                }
                                return;
                        }
                        if (current->recvq.length() > (unsigned)NetBufferSize)
                        {
                                if (current->registered == 7)
                                {
                                        kill_link(current,"RecvQ exceeded");
                                }
                                else
                                {
                                        WriteOpers("*** Excess flood from %s",current->ip);
                                        log(DEFAULT,"Excess flood from: %s",current->ip);
                                        add_zline(120,ServerName,"Flood from unregistered connection",current->ip);
                                        apply_lines();
                                }
                                return;
                        }
                        // while there are complete lines to process...
                        while (current->BufferIsReady())
                        {
                                floodlines++;
                                if (TIME > current->reset_due)
                                {
                                        current->reset_due = TIME + current->threshold;
                                        current->lines_in = 0;
                                }
                                current->lines_in++;
                                if (current->lines_in > current->flood)
                                {
                                        log(DEFAULT,"Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
                                        WriteOpers("*** Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
                                        kill_link(current,"Excess flood");
                                        return;
                                }
                                if ((floodlines > current->flood) && (current->flood != 0))
                                {
                                        if (current->registered == 7)
                                        {
						log(DEFAULT,"Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
                                                WriteOpers("*** Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
                                                kill_link(current,"Excess flood");
                                        }
                                        else
                                        {
                                                add_zline(120,ServerName,"Flood from unregistered connection",current->ip);
                                                apply_lines();
                                        }
                                        return;
                                }
                                char sanitized[MAXBUF];
                                // use GetBuffer to copy single lines into the sanitized string
                                std::string single_line = current->GetBuffer();
                                current->bytes_in += single_line.length();
                                current->cmds_in++;
                                if (single_line.length()>512)
                                {
                                        log(DEFAULT,"Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
                                        WriteOpers("*** Excess flood from: %s!%s@%s",current->nick,current->ident,current->host);
                                        kill_link(current,"Excess flood");
                                        return;
                                }
                                strlcpy(sanitized,single_line.c_str(),MAXBUF);
                                if (*sanitized)
                                {
                                        userrec* old_comp = fd_ref_table[currfd];
                                        // we're gonna re-scan to check if the nick is gone, after every
                                        // command - if it has, we're gonna bail
                                        process_buffer(sanitized,current);
                                        // look for the user's record in case it's changed... if theyve quit,
                                        // we cant do anything more with their buffer, so bail.
                                        // there used to be an ugly, slow loop here. Now we have a reference
                                        // table, life is much easier (and FASTER)
                                        userrec* new_comp = fd_ref_table[currfd];
                                        if ((currfd < 0) || (!fd_ref_table[currfd]) || (old_comp != new_comp))
                                                return;
                                }
                        }
                        return;
                }
                if ((result == -1) && (errno != EAGAIN) && (errno != EINTR))
                {
                        log(DEBUG,"killing: %s",cu->nick);
                        kill_link(cu,strerror(errno));
                        return;
                }
        }
        // result EAGAIN means nothing read
	else if (result == EAGAIN)
        {
        }
        else if (result == 0)
        {
                log(DEBUG,"InspIRCd: Exited: %s",cu->nick);
                kill_link(cu,"Client exited");
                log(DEBUG,"Bailing from client exit");
                return;
        }
}

void DoBackgroundUserStuff()
{
        for (user_hash::iterator count2 = clientlist.begin(); count2 != clientlist.end(); count2++)
        {
                userrec* curr = NULL;
                if (count2->second)
                        curr = count2->second;
                if ((long)curr == -1)
                        return;

                if ((curr) && (curr->fd != 0))
                {
                        int currfd = curr->fd;
                        // we don't check the state of remote users.
                        if ((currfd != -1) && (currfd != FD_MAGIC_NUMBER))
                        {
                                curr->FlushWriteBuf();
                                if (curr->GetWriteError() != "")
                                {
                                        log(DEBUG,"InspIRCd: write error: %s",curr->GetWriteError().c_str());
                                        kill_link(curr,curr->GetWriteError().c_str());
                                        return;
                                }
                                // registration timeout -- didnt send USER/NICK/HOST in the time specified in
                                // their connection class.
                                if (((unsigned)TIME > (unsigned)curr->timeout) && (curr->registered != 7))
                                {
                                        log(DEBUG,"InspIRCd: registration timeout: %s",curr->nick);
                                        kill_link(curr,"Registration timeout");
                                        return;
                                }
                                if ((TIME > curr->signon) && (curr->registered == 3) && (AllModulesReportReady(curr)))
                                {
                                        log(DEBUG,"signon exceed, registered=3, and modules ready, OK: %d %d",TIME,curr->signon);
                                        curr->dns_done = true;
                                        statsDnsBad++;
                                        FullConnectUser(curr);
                                        if (fd_ref_table[currfd] != curr) // something changed, bail pronto
	                                        return;
                                 }
                                 if ((curr->dns_done) && (curr->registered == 3) && (AllModulesReportReady(curr)))
                                 {
                                       log(DEBUG,"dns done, registered=3, and modules ready, OK");
                                       FullConnectUser(curr);
                                       if (fd_ref_table[currfd] != curr) // something changed, bail pronto
                                                return;
                                 }
                                 if ((TIME > curr->nping) && (isnick(curr->nick)) && (curr->registered == 7))
                                 {
                                       if ((!curr->lastping) && (curr->registered == 7))
                                       {
	                                       log(DEBUG,"InspIRCd: ping timeout: %s",curr->nick);
                                               kill_link(curr,"Ping timeout");
                                               return;
                                       }
                                       Write(curr->fd,"PING :%s",ServerName);
                                       log(DEBUG,"InspIRCd: pinging: %s",curr->nick);
                                       curr->lastping = 0;
                                       curr->nping = TIME+curr->pingmax;       // was hard coded to 120
                                 }
                         }
                 }
         }
}

void OpenLog(char** argv, int argc)
{
	std::string logpath = GetFullProgDir(argv,argc) + "/ircd.log";
        log_file = fopen(logpath.c_str(),"a+");
        if (!log_file)
        {
                printf("ERROR: Could not write to logfile %s, bailing!\n\n",logpath.c_str());
                Exit(ERROR);
	}
#ifdef IS_CYGWIN
        printf("Logging to ircd.log...\n");
#else
        printf("Logging to %s...\n",logpath.c_str());
#endif
}

void CheckRoot()
{
	if (geteuid() == 0)
	{
		printf("WARNING!!! You are running an irc server as ROOT!!! DO NOT DO THIS!!!\n\n");
		log(DEFAULT,"InspIRCd: startup: not starting with UID 0!");
		Exit(ERROR);
	}
}

int InspIRCd(char** argv, int argc)
{
	struct sockaddr_in client,server;
	char addrs[MAXBUF][255];
	int incomingSockfd;
	socklen_t length;
	int count = 0;
	int clientportcount = 0;
	char configToken[MAXBUF], Addr[MAXBUF], Type[MAXBUF];

	OpenLog(argv, argc);
	CheckRoot();
	SetupCommandTable();
	ReadConfig(true,NULL);
	AddServerName(ServerName);
	
	if (DieValue[0])
	{ 
		printf("WARNING: %s\n\n",DieValue);
		log(DEFAULT,"Ut-Oh, somebody didn't read their config file: '%s'",DieValue);
		exit(0); 
	}  
	log(DEBUG,"InspIRCd: startup: read config");

	for (count = 0; count < ConfValueEnum("bind",&config_f); count++)
	{
		ConfValue("bind","port",count,configToken,&config_f);
		ConfValue("bind","address",count,Addr,&config_f);
		ConfValue("bind","type",count,Type,&config_f);
		if (!strcmp(Type,"servers"))
		{
			// modules handle this bind type now.
		}
		else
		{
			ports[clientportcount] = atoi(configToken);
			strlcpy(addrs[clientportcount],Addr,256);
			clientportcount++;
		}
		log(DEBUG,"InspIRCd: startup: read binding %s:%s [%s] from config",Addr,configToken, Type);
	}
	portCount = clientportcount;
  
	log(DEBUG,"InspIRCd: startup: read %lu total client ports",(unsigned long)portCount);

	printf("\n");
	
	/* BugFix By Craig! :p */
	MODCOUNT = -1;
	for (count = 0; count < ConfValueEnum("module",&config_f); count++)
	{
		ConfValue("module","name",count,configToken,&config_f);
		printf("Loading module... \033[1;32m%s\033[0m\n",configToken);
		if (!LoadModule(configToken))
		{
			log(DEFAULT,"Exiting due to a module loader error.");
			printf("\nThere was an error loading a module: %s\n\nYou might want to do './inspircd start' instead of 'bin/inspircd'\n\n",ModuleError());
			Exit(0);
		}
	}
	log(DEFAULT,"Total loaded modules: %lu",(unsigned long)MODCOUNT+1);
	
	startup_time = time(NULL);
	  
	char PID[MAXBUF];
	ConfValue("pid","file",0,PID,&config_f);
	// write once here, to try it out and make sure its ok
	WritePID(PID);

	log(VERBOSE,"InspIRCd: startup: portCount = %lu", (unsigned long)portCount);
	
	for (count = 0; count < portCount; count++)
	{
		if ((openSockfd[boundPortCount] = OpenTCPSocket()) == ERROR)
		{
			log(DEBUG,"InspIRCd: startup: bad fd %lu",(unsigned long)openSockfd[boundPortCount]);
			return(ERROR);
		}
		if (BindSocket(openSockfd[boundPortCount],client,server,ports[count],addrs[count]) == ERROR)
		{
			log(DEFAULT,"InspIRCd: startup: failed to bind port %lu",(unsigned long)ports[count]);
		}
		else	/* well we at least bound to one socket so we'll continue */
		{
			boundPortCount++;
		}
	}
	
	log(DEBUG,"InspIRCd: startup: total bound ports %lu",(unsigned long)boundPortCount);
	  
	/* if we didn't bind to anything then abort */
	if (boundPortCount == 0)
	{
		log(DEFAULT,"InspIRCd: startup: no ports bound, bailing!");
		printf("\nERROR: Was not able to bind any of %lu ports! Please check your configuration.\n\n", (unsigned long)portCount);
		return (ERROR);
	}
	

        printf("\nInspIRCd is now running!\n");

        if (nofork)
        {
                log(VERBOSE,"Not forking as -nofork was specified");
        }
        else
        {
                if (DaemonSeed() == ERROR)
                {
                        log(DEFAULT,"InspIRCd: startup: can't daemonise");
                        printf("ERROR: could not go into daemon mode. Shutting down.\n");
                        Exit(ERROR);
                }
        }

	SE = new SocketEngine();

	/* Add the listening sockets used for client inbound connections
	 * to the socket engine
	 */
	for (count = 0; count < portCount; count++)
		SE->AddFd(openSockfd[count],true,X_LISTEN);

	std::vector<int> activefds;

	WritePID(PID);
	bool expire_run = false;

	/* main loop, this never returns */
	for (;;)
	{
#ifdef _POSIX_PRIORITY_SCHEDULING
		sched_yield();
#endif
		// we only read time() once per iteration rather than tons of times!
		OLDTIME = TIME;
		TIME = time(NULL);

#ifndef THREADED_DNS
		dns_poll();
#endif

		// *FIX* Instead of closing sockets in kill_link when they receive the ERROR :blah line, we should queue
		// them in a list, then reap the list every second or so.
		if (((TIME % 5) == 0) && (!expire_run))
		{
			expire_lines();
			FOREACH_MOD OnBackgroundTimer(TIME);
			expire_run = true;
			continue;
		}
		if ((TIME % 5) == 1)
			expire_run = false;
		
		DoBackgroundUserStuff();

		SE->Wait(activefds);

		for (unsigned int activefd = 0; activefd < activefds.size(); activefd++)
		{
			if (SE->GetType(activefds[activefd]) == X_ESTAB_CLIENT)
			{
				log(DEBUG,"Got a ready socket of type X_ESTAB_CLIENT");
				userrec* cu = fd_ref_table[activefds[activefd]];
				if (cu)
				{
					/* It's a user */
					ProcessUser(cu);
				}
			}
			else if (SE->GetType(activefds[activefd]) == X_ESTAB_MODULE)
			{
				log(DEBUG,"Got a ready socket of type X_ESTAB_MODULE");
				unsigned int numsockets = module_sockets.size();
				for (std::vector<InspSocket*>::iterator a = module_sockets.begin(); a < module_sockets.end(); a++)
				{
					InspSocket* s = (InspSocket*)*a;
					if ((s) && (s->GetFd() == activefds[activefd]))
					{
						if (!s->Poll())
						{
							log(DEBUG,"Socket poll returned false, close and bail");
							SE->DelFd(s->GetFd());
							s->Close();
							module_sockets.erase(a);
							delete s;
							break;
						}
						if (module_sockets.size() != numsockets) break;
					}
				}
			}
			else if (SE->GetType(activefds[activefd]) == X_ESTAB_DNS)
			{
				log(DEBUG,"Got a ready socket of type X_ESTAB_DNS");
			}
			else if (SE->GetType(activefds[activefd]) == X_LISTEN)
			{
				log(DEBUG,"Got a ready socket of type X_LISTEN");
				/* It maybe a listener */
				for (count = 0; count < boundPortCount; count++)
				{
					if (activefds[activefd] == openSockfd[count])
					{
						char target[MAXBUF], resolved[MAXBUF];
						length = sizeof (client);
						incomingSockfd = accept (openSockfd[count], (struct sockaddr *) &client, &length);
						log(DEBUG,"Accepted socket %d",incomingSockfd);
						strlcpy (target, (char *) inet_ntoa (client.sin_addr), MAXBUF);
						strlcpy (resolved, target, MAXBUF);
						if (incomingSockfd >= 0)
						{
							FOREACH_MOD OnRawSocketAccept(incomingSockfd, resolved, ports[count]);
							statsAccept++;
							AddClient(incomingSockfd, resolved, ports[count], false, inet_ntoa (client.sin_addr));
							log(DEBUG,"Adding client on port %lu fd=%lu",(unsigned long)ports[count],(unsigned long)incomingSockfd);
						}
						else
						{
							WriteOpers("*** WARNING: accept() failed on port %lu (%s)",(unsigned long)ports[count],target);
							log(DEBUG,"accept failed: %lu",(unsigned long)ports[count]);
							statsRefused++;
						}
					}
				}
			}
		}

	}
	/* This is never reached -- we hope! */
	return 0;
}

