#include "inspircd.h"
#include "listmode.h"

class ListModeExtendMode : public ParamMode<ListModeExtendMode, LocalStringExt>
{
public:
	ListModeExtendMode(Module* Creator)
		: ParamMode<ListModeExtendMode, LocalStringExt>(Creator, "listmodeextend", 'E')
	{
		this->oper = true;
		syntax = "<listmodes>";
	}

	ModeAction OnSet(User* source, Channel* channel, std::string& param) CXX11_OVERRIDE
	{
		if (IS_LOCAL(source))
		{
			if (param == "*")
			{
				if (channel->GetModeParameter(this) == "*")
				{
					return MODEACTION_DENY;
				}
			}
			else
			{
				std::string newParam;
				for (char c : param)
				{
					ModeHandler* handler = ServerInstance->Modes->FindMode(c, MODETYPE_CHANNEL);

					if (handler && handler->IsListMode() && !handler->IsPrefixMode() && newParam.find(c) == std::string::npos) {
						newParam.push_back(handler->GetModeChar());
					}
				}
				std::sort(newParam.begin(), newParam.end());

				if (channel->GetModeParameter(this) == newParam)
				{
					return MODEACTION_DENY;
				}
				param = newParam;
			}
		}

		ext.set(channel, param);

		for (ListModeBase* lm : ServerInstance->Modes->GetListModes())
		{
			lm->InvalidateLimit(channel);
		}
		return MODEACTION_ALLOW;
	}

	void OnUnset(User* source, Channel* target) CXX11_OVERRIDE
	{
		for (ListModeBase* lm : ServerInstance->Modes->GetListModes())
		{
			lm->InvalidateLimit(target);
		}
	}

	void SerializeParam(Channel* chan, const std::string* str, std::string& out)
	{
		out += *str;
	}
};

class ListModeExtendModule : public Module
{
	ListModeExtendMode lmem;

public:
	ListModeExtendModule()
		: lmem(this)
	{}

	Version GetVersion() CXX11_OVERRIDE
	{
		return Version("Provides channel mode +E which allows extending list modes beyond the normal limit.", VF_COMMON);
	}
};

MODULE_INIT(ListModeExtendModule)
