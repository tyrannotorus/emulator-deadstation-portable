// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// The PSP's HTTP library: there is no internet play here, so a game's calls go unanswered.
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceHttp.h"

void __HttpInit() {
}

void __HttpShutdown() {
}

const HLEFunction sceHttp[] = {
	{0XAB1ABE07, nullptr, "sceHttpInit",                    'i', "i"     },
	{0XD1C8945E, nullptr, "sceHttpEnd",                     'i', ""      },
	{0XA6800C34, nullptr, "sceHttpInitCache",               'i', "i"     },
	{0X78B54C09, nullptr, "sceHttpEndCache",                'i', ""      },
	{0X59E6D16F, nullptr, "sceHttpEnableCache",             'i', "i"     },
	{0XCCBD167A, nullptr, "sceHttpDisableCache",            'i', "i"     },
	{0XD70D4847, nullptr, "sceHttpGetProxy",                'x', "xxxxxx"},
	{0X4CC7D78F, nullptr, "sceHttpGetStatusCode",           'i', "ix"    },
	{0XEDEEB999, nullptr, "sceHttpReadData",                'i', "ixx"   },
	{0XBB70706F, nullptr, "sceHttpSendRequest",             'i', "ixx"   },
	{0XA5512E01, nullptr, "sceHttpDeleteRequest",           'i', "i"     },
	{0X15540184, nullptr, "sceHttpDeleteHeader",            'i', "is"    },
	{0X5152773B, nullptr, "sceHttpDeleteConnection",        'i', "i"     },
	{0X8ACD1F73, nullptr, "sceHttpSetConnectTimeOut",       'i', "ix"    },
	{0X9988172D, nullptr, "sceHttpSetSendTimeOut",          'i', "ix"    },
	{0XF0F46C62, nullptr, "sceHttpSetProxy",                'x', "xxxxx" },
	{0X0DAFA58F, nullptr, "sceHttpEnableCookie",            'i', "i"     },
	{0X78A0D3EC, nullptr, "sceHttpEnableKeepAlive",         'i', "i"     },
	{0X0B12ABFB, nullptr, "sceHttpDisableCookie",           'i', "i"     },
	{0XC7EF2559, nullptr, "sceHttpDisableKeepAlive",        'i', "i"     },
	{0XE4D21302, nullptr, "sceHttpsInit",                   'i', "iiii"  },
	{0XF9D8EB63, nullptr, "sceHttpsEnd",                    'i', ""      },
	{0X47347B50, nullptr, "sceHttpCreateRequest",           'i', "iisX"  },
	{0X8EEFD953, nullptr, "sceHttpCreateConnection",        'i', "issxi" },
	{0XD081EC8F, nullptr, "sceHttpGetNetworkErrno",         'i', "ix"    },
	{0X3EABA285, nullptr, "sceHttpAddExtraHeader",          'i', "issi"  },
	{0XC10B6BD9, nullptr, "sceHttpAbortRequest",            'i', "i"     },
	{0XFCF8C055, nullptr, "sceHttpDeleteTemplate",          'i', "i"     },
	{0XF49934F6, nullptr, "sceHttpSetMallocFunction",       'i', "xxx"   },
	{0X03D9526F, nullptr, "sceHttpSetResolveRetry",         'i', "ii"    },
	{0X47940436, nullptr, "sceHttpSetResolveTimeOut",       'i', "ix"    },
	{0X2A6C3296, nullptr, "sceHttpSetAuthInfoCB",           'i', "ix"    },
	{0X0809C831, nullptr, "sceHttpEnableRedirect",          'i', "i"     },
	{0X9FC5F10D, nullptr, "sceHttpEnableAuth",              'i', "i"     },
	{0X1A0EBB69, nullptr, "sceHttpDisableRedirect",         'i', "i"     },
	{0XAE948FEE, nullptr, "sceHttpDisableAuth",             'i', "i"     },
	{0X76D1363B, nullptr, "sceHttpSaveSystemCookie",        'i', ""      },
	{0X87797BDD, nullptr, "sceHttpsLoadDefaultCert",        'i', "ii"    },
	{0XF1657B22, nullptr, "sceHttpLoadSystemCookie",        'i', ""      },
	{0X9B1F1F36, nullptr, "sceHttpCreateTemplate",          'i', "sii"   },
	{0XB509B09E, nullptr, "sceHttpCreateRequestWithURL",    'i', "iisX"  },
	{0XCDF8ECB9, nullptr, "sceHttpCreateConnectionWithURL", 'i', "isi"   },
	{0X1F0FC3E3, nullptr, "sceHttpSetRecvTimeOut",          'i', "ix"    },
	{0XDB266CCF, nullptr, "sceHttpGetAllHeader",            'i', "ixx"   },
	{0X0282A3BD, nullptr, "sceHttpGetContentLength",        'i', "ix"    },
	{0X7774BF4C, nullptr,                                    "sceHttpAddCookie",               '?', ""      },
	{0X68AB0F86, nullptr, "sceHttpsInitWithPath",           'i', "iii"   },
	{0XB3FAF831, nullptr, "sceHttpsDisableOption",          'i', "i"     },
	{0X2255551E, nullptr,                                    "sceHttpGetNetworkPspError",      '?', ""      },
	{0XAB1540D5, nullptr,                                    "sceHttpsGetSslError",            '?', ""      },
	{0XA4496DE5, nullptr, "sceHttpSetRedirectCallback",     'i', "ixx"   },
	{0X267618F4, nullptr, "sceHttpSetAuthInfoCallback",     'i', "ixx"   },
	{0X569A1481, nullptr, "sceHttpsSetSslCallback",         'i', "ixx"   },
	{0XBAC31BF1, nullptr,                                    "sceHttpsEnableOption",           '?', ""      },
};

void Register_sceHttp()
{
	RegisterHLEModule("sceHttp",ARRAY_SIZE(sceHttp),sceHttp);
}
