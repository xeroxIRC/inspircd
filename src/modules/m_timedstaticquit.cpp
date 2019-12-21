#include "inspircd.h"

class TimedStaticQuitModule : public Module
{
	std::string staticmsg;
	unsigned long maxage;

public:
	TimedStaticQuitModule()
		: staticmsg("")
		, maxage(0)
	{}

	void ReadConfig(ConfigStatus& status) CXX11_OVERRIDE
	{
		ConfigTag* tag = ServerInstance->Config->ConfValue("timedstaticquit");
		staticmsg = tag->getString("message", "Client Quit");
		maxage = tag->getDuration("maxage", 300);
	}

	ModResult OnPreCommand(std::string& command, CommandBase::Params& parameters, LocalUser* user, bool validated) CXX11_OVERRIDE
	{
		if (validated && command == "QUIT" && (IS_LOCAL(user)))
		{
			time_t now = ServerInstance->Time();
			if ((now - user->signon) <= (time_t)maxage)
			{
				ServerInstance->Users->QuitUser(user, staticmsg);
				return MOD_RES_DENY;
			}
		}
		return MOD_RES_PASSTHRU;
	}

	Version GetVersion() CXX11_OVERRIDE
	{
		return Version("Provides the feature where users who have not been connected for long enough may not use a custom quit message.");
	}

	void Prioritize() CXX11_OVERRIDE
	{
		// We eat the quit command so we go last
		ServerInstance->Modules->SetPriority(this, PRIORITY_LAST);
	}
};

MODULE_INIT(TimedStaticQuitModule)
