#include "inspircd.h"
#include "numeric.h"

class ChannameWhitelistModule : public Module
{
	std::string allowed_chars;

public:
	ChannameWhitelistModule()
		: allowed_chars("")
	{}

	void ReadConfig(ConfigStatus& status) CXX11_OVERRIDE
	{
		ConfigTag* tag = ServerInstance->Config->ConfValue("channamewl");

		allowed_chars = tag->getString("allowchars");
	}

	ModResult OnUserPreJoin(LocalUser* user, Channel* channel, const std::string& cname, std::string& privs, const std::string& keygiven) CXX11_OVERRIDE
	{
		for (char c : cname)
		{
			if (c == '#') continue; // We just consider # as always whitelisted no matter what, because channels cannot exist otherwise.
			if (allowed_chars.find(c) == std::string::npos) {
				user->WriteNumeric(ERR_NOSUCHCHANNEL, cname, "Channel name contains invalid characters.");
				return MOD_RES_DENY;
			}
		}
		return MOD_RES_PASSTHRU;
	}

	Version GetVersion() CXX11_OVERRIDE
	{
		return Version("Provides a whitelist feature for channels being joined. This is a solution for the fake channels with zero-width spaces and cyrillic (or other) look-alikes.");
	}
};

MODULE_INIT(ChannameWhitelistModule)
