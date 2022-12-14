/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2016 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "machine.h"
#include "machine_interface.h"
#include "network/common.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/message_handler.h"
#include "network/bundle_broadcast.h"
#include "helper/sys_info.h"
#include "thread/threadpool.h"
#include "server/component_active_report_handler.h"

#include "../baseappmgr/baseappmgr_interface.h"
#include "../cellappmgr/cellappmgr_interface.h"
#include "../baseapp/baseapp_interface.h"
#include "../cellapp/cellapp_interface.h"
#include "../dbmgr/dbmgr_interface.h"
#include "../loginapp/loginapp_interface.h"
#include "../tools/logger/logger_interface.h"
#include "../../server/tools/interfaces/interfaces_interface.h"
#include "../../server/tools/bots/bots_interface.h"

namespace KBEngine{
	
ServerConfig g_serverConfig;
KBE_SINGLETON_INIT(Machine);

//-------------------------------------------------------------------------------------
Machine::Machine(Network::EventDispatcher& dispatcher, 
				 Network::NetworkInterface& ninterface, 
				 COMPONENT_TYPE componentType,
				 COMPONENT_ID componentID):
	ServerApp(dispatcher, ninterface, componentType, componentID),
	broadcastAddr_(0),
	ep_(),
	epBroadcast_(),
	epLocal_(),
	pEPPacketReceiver_(NULL),
	pEBPacketReceiver_(NULL),
	pEPLocalPacketReceiver_(NULL),
	localuids_()
{
	SystemInfo::getSingleton().getCPUPer();
}

//-------------------------------------------------------------------------------------
Machine::~Machine()
{
	ep_.close();
	epBroadcast_.close();
	epLocal_.close();
	localuids_.clear();

	SAFE_RELEASE(pEPPacketReceiver_);
	SAFE_RELEASE(pEBPacketReceiver_);
	SAFE_RELEASE(pEPLocalPacketReceiver_);
}

//-------------------------------------------------------------------------------------
void Machine::onBroadcastInterface(Network::Channel* pChannel, int32 uid, std::string& username,
								   COMPONENT_TYPE componentType, COMPONENT_ID componentID, COMPONENT_ID componentIDEx, 
								   COMPONENT_ORDER globalorderid, COMPONENT_ORDER grouporderid,
									uint32 intaddr, uint16 intport,
									uint32 extaddr, uint16 extport, std::string& extaddrEx, uint32 pid,
									float cpu, float mem, uint32 usedmem, int8 state, uint32 machineID, uint64 extradata,
									uint64 extradata1, uint64 extradata2, uint64 extradata3, uint32 backRecvAddr, uint16 backRecvPort)
{
	// ????????????????????????????????????????????????????????????????????????????????
	const Components::ComponentInfos* pinfos = Components::getSingleton().findComponent(componentID);
	if(pinfos && isGameServerComponentType((COMPONENT_TYPE)componentType) && checkComponentUsable(pinfos, false, true))
	{
		if(pinfos->pid != pid || pinfos->pIntAddr->ip != intaddr ||
			username != pinfos->username || uid != pinfos->uid)
		{
			Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();

			MachineInterface::onBroadcastInterfaceArgs24::staticAddToBundle((*pBundle), pinfos->uid,
				pinfos->username, pinfos->componentType, pinfos->cid, componentIDEx, pinfos->globalOrderid, pinfos->groupOrderid, 
				pinfos->pIntAddr->ip, pinfos->pIntAddr->port,
				pinfos->pExtAddr->ip, pinfos->pExtAddr->port, pinfos->externalAddressEx, pinfos->pid, pinfos->cpu, pinfos->mem, pinfos->usedmem, 
				(int8)pinfos->state, KBEngine::getProcessPID(), pinfos->extradata, pinfos->extradata1, pinfos->extradata2, pinfos->extradata3, 0, 0);

			if(backRecvAddr != 0 && backRecvPort != 0)
			{
				Network::EndPoint ep;
				ep.socket(SOCK_DGRAM);

				if (!ep.good())
				{
					ERROR_MSG("Machine::onBroadcastInterface: Failed to create socket.\n");
					return;
				}

				ep.sendto(pBundle, backRecvPort, backRecvAddr);
				Network::Bundle::ObjPool().reclaimObject(pBundle);
			}
			else
			{
				pChannel->send(pBundle);
			}

			return;
		}
	}

	// ????????????????????
	if(this->networkInterface().intaddr().ip == intaddr ||
				this->networkInterface().extaddr().ip == intaddr)
	{
		pinfos = Components::getSingleton().findComponent((COMPONENT_TYPE)componentType, uid, componentID);
		if(pinfos)
		{
			if(checkComponentUsable(pinfos, false, true))
			{
				WARNING_MSG(fmt::format("Machine::onBroadcastInterface: {} has running, pid={}, uid={}!\n", 
					COMPONENT_NAME_EX((COMPONENT_TYPE)componentType), pid, uid));

				return;
			}
		}

		// ??????????????????????machine
		if(componentType == MACHINE_TYPE)
		{
			WARNING_MSG("Machine::onBroadcastInterface: machine has running!\n");
			return;
		}
	
		std::vector<int32>::iterator iter = std::find(localuids_.begin(), localuids_.end(), uid);
		if(iter == localuids_.end())
		{
			INFO_MSG(fmt::format("Machine::onBroadcastInterface: added newUID {0}.\n", uid));
			localuids_.push_back(uid);
		}

		INFO_MSG(fmt::format("Machine::onBroadcastInterface[{0}]: uid:{1}, username:{2}, componentType:{3}, "
			"componentID:{4}, globalorderid={9}, grouporderid={10}, pid:{11}, intaddr:{5}, intport:{6}, extaddr:{7}, extport:{8}.\n",
				pChannel->c_str(),
				uid,
				username.c_str(),
				COMPONENT_NAME_EX((COMPONENT_TYPE)componentType),
				componentID,
				inet_ntoa((struct in_addr&)intaddr),
				ntohs(intport),
				(extaddr != 0 ? inet_ntoa((struct in_addr&)extaddr) : "nonsupport"),
				ntohs(extport),
				((int32)globalorderid),
				((int32)grouporderid),
				pid));

		Components::getSingleton().addComponent(uid, username.c_str(), 
			(KBEngine::COMPONENT_TYPE)componentType, componentID, globalorderid, grouporderid, intaddr, intport, extaddr, extport, extaddrEx,
			pid, cpu, mem, usedmem, extradata, extradata1, extradata2, extradata3);
	}
}

//-------------------------------------------------------------------------------------
void Machine::onFindInterfaceAddr(Network::Channel* pChannel, int32 uid, std::string& username, COMPONENT_TYPE componentType, COMPONENT_ID componentID,
								  COMPONENT_TYPE findComponentType, uint32 finderAddr, uint16 finderRecvPort)
{
	KBEngine::COMPONENT_TYPE tfindComponentType = (KBEngine::COMPONENT_TYPE)findComponentType;
	KBEngine::COMPONENT_TYPE tComponentType = (KBEngine::COMPONENT_TYPE)componentType;

	// ????????guiconsole??????, uid????????????????????uid??????????
	if(tComponentType != CONSOLE_TYPE)
	{
		std::vector<int32>::iterator iter = std::find(localuids_.begin(), localuids_.end(), uid);
		if(iter == localuids_.end())
			return;
	}

	INFO_MSG(fmt::format("Machine::onFindInterfaceAddr[{0}]: uid:{1}, username:{2}, "
			"componentType:{3}, componentID:{7}, find:{4}, finderaddr:{5}, finderRecvPort:{6}.\n",
		pChannel->c_str(), uid, username.c_str(), 
		COMPONENT_NAME_EX((COMPONENT_TYPE)componentType),
		COMPONENT_NAME_EX((COMPONENT_TYPE)findComponentType),
		inet_ntoa((struct in_addr&)finderAddr), ntohs(finderRecvPort),
		componentID));

	Network::EndPoint ep;
	ep.socket(SOCK_DGRAM);

	if (!ep.good())
	{
		ERROR_MSG("Machine::onFindInterfaceAddr: Failed to create socket.\n");
		return;
	}
	
	Components::COMPONENTS& components = Components::getSingleton().getComponents(tfindComponentType);
	Components::COMPONENTS::iterator iter = components.begin();

	bool found = false;
	Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();

	for(; iter != components.end(); )
	{
		if(tComponentType != CONSOLE_TYPE)
		{
			if((*iter).uid != uid)
			{
				++iter;
				continue;
			}
		}

		const Components::ComponentInfos* pinfos = &(*iter);
		
		bool usable = checkComponentUsable(pinfos, false, false);

		if(usable)
		{
			if(this->networkInterface().intaddr().ip == pinfos->pIntAddr->ip ||
				this->networkInterface().extaddr().ip == pinfos->pIntAddr->ip)
			{
				found = true;

				MachineInterface::onBroadcastInterfaceArgs24::staticAddToBundle((*pBundle), pinfos->uid, 
					pinfos->username, findComponentType, pinfos->cid, componentID, pinfos->globalOrderid, pinfos->groupOrderid, 
					pinfos->pIntAddr->ip, pinfos->pIntAddr->port,
					pinfos->pExtAddr->ip, pinfos->pExtAddr->port, pinfos->externalAddressEx, pinfos->pid, pinfos->cpu, pinfos->mem, pinfos->usedmem, 
					(int8)pinfos->state, KBEngine::getProcessPID(), pinfos->extradata, pinfos->extradata1, pinfos->extradata2, pinfos->extradata3, 0, 0);
			}

			++iter;
		}
		else
		{
			WARNING_MSG(fmt::format("Machine::onFindInterfaceAddr: {}[{}] invalid, erase {}.\n",
				COMPONENT_NAME_EX(pinfos->componentType),
				pinfos->cid,
				COMPONENT_NAME_EX(pinfos->componentType)));

			iter = components.erase(iter);
		}
	}

	if(!found)
	{
		// ?????????????? ??uid??????????????????????????????
		// ??????????????????????????
		if(tComponentType == CONSOLE_TYPE)
		{
			std::vector<int32>::iterator iter = std::find(localuids_.begin(), localuids_.end(), uid);
			if(iter == localuids_.end())
				return;
		}

		WARNING_MSG(fmt::format("Machine::onFindInterfaceAddr: {} not found {}.\n",
			COMPONENT_NAME_EX(tComponentType),
			COMPONENT_NAME_EX(tfindComponentType)));

		MachineInterface::onBroadcastInterfaceArgs24::staticAddToBundle((*pBundle), KBEngine::getUserUID(), 
			"", UNKNOWN_COMPONENT_TYPE, 0, componentID, -1, -1, 0, 0, 0, 0, "", 0, 0.f, 0.f, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	}

	if(finderAddr != 0 && finderRecvPort != 0)
	{
		ep.sendto(pBundle, finderRecvPort, finderAddr);
		Network::Bundle::ObjPool().reclaimObject(pBundle);
	}
	else
	{
		pChannel->send(pBundle);
	}
}

//-------------------------------------------------------------------------------------
bool Machine::checkComponentUsable(const Components::ComponentInfos* info, bool getdatas, bool autoerase)
{
	bool ret = false;
	
	if(!getdatas)
		ret = Components::getSingleton().lookupLocalComponentRunning(info->pid) != NULL;
	else
		ret = Components::getSingleton().updateComponentInfos(info);

	// ????????????????????????????????????
	if(!ret && autoerase)
		Components::getSingleton().delComponent(info->uid, info->componentType, info->cid);

	return ret;
}

//-------------------------------------------------------------------------------------
void Machine::onQueryAllInterfaceInfos(Network::Channel* pChannel, int32 uid, std::string& username, uint16 finderRecvPort)
{
	// uid??????????????????uid??????????
	if(uid > 0)
	{
		std::vector<int32>::iterator iter = std::find(localuids_.begin(), localuids_.end(), uid);
		if(iter == localuids_.end())
			return;
	}

	INFO_MSG(fmt::format("Machine::onQueryAllInterfaceInfos[{}]: uid:{}, username:{}, "
			"finderRecvPort:{}.\n",
		pChannel->c_str(), uid, username.c_str(),
		ntohs(finderRecvPort)));

	Network::EndPoint ep;
	ep.socket(SOCK_DGRAM);

	if (!ep.good())
	{
		ERROR_MSG("Machine::onQueryAllInterfaceInfos: Failed to create socket.\n");
		return;
	}

	{
		Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();
		
		uint64 cidex = 0;
		float cpu = SystemInfo::getSingleton().getCPUPer();
		uint64 totalmem = SystemInfo::getSingleton().getMemInfos().total;
		uint64 totalusedmem = SystemInfo::getSingleton().getMemInfos().used;

		MachineInterface::onBroadcastInterfaceArgs24::staticAddToBundle((*pBundle), getUserUID(), getUsername(), 
			g_componentType, g_componentID, cidex, g_componentGlobalOrder, g_componentGroupOrder,
			networkInterface_.intaddr().ip, networkInterface_.intaddr().port,
			networkInterface_.extaddr().ip, networkInterface_.extaddr().port, "", getProcessPID(),
			cpu, float((totalusedmem * 1.0 / totalmem) * 100.0), (uint32)SystemInfo::getSingleton().getMemUsedByPID(), 0, 
			getProcessPID(), totalmem, totalusedmem, uint64(SystemInfo::getSingleton().getCPUPerByPID() * 100), 0, 0, 0);

		if(finderRecvPort != 0)
		{
			ep.sendto(pBundle, finderRecvPort, pChannel->addr().ip);
			Network::Bundle::ObjPool().reclaimObject(pBundle);
		}
		else
		{
			pChannel->send(pBundle);
		}
	}

	int i = 0;

	while(ALL_SERVER_COMPONENT_TYPES[i] != UNKNOWN_COMPONENT_TYPE)
	{
		COMPONENT_TYPE findComponentType = ALL_SERVER_COMPONENT_TYPES[i++];
		Components::COMPONENTS& components = Components::getSingleton().getComponents(findComponentType);
		Components::COMPONENTS::iterator iter = components.begin();

		for(; iter != components.end(); )
		{
			if(uid > 0 && (*iter).uid != uid)
			{
				++iter;
				continue;
			}

			const Components::ComponentInfos* pinfos = &(*iter);
			
			bool islocal = this->networkInterface().intaddr().ip == pinfos->pIntAddr->ip ||
					this->networkInterface().extaddr().ip == pinfos->pIntAddr->ip;

			bool usable = checkComponentUsable(pinfos, true, false);

			if(usable)
			{
				if(islocal)
				{
					Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();
					
					MachineInterface::onBroadcastInterfaceArgs24::staticAddToBundle((*pBundle), pinfos->uid, 
						pinfos->username, findComponentType, pinfos->cid, pinfos->cid, pinfos->globalOrderid, pinfos->groupOrderid, 
						pinfos->pIntAddr->ip, pinfos->pIntAddr->port,
						pinfos->pExtAddr->ip, pinfos->pExtAddr->port, pinfos->externalAddressEx, pinfos->pid, 
						pinfos->cpu, pinfos->mem, pinfos->usedmem, 
						(int8)pinfos->state, KBEngine::getProcessPID(), pinfos->extradata, pinfos->extradata1, pinfos->extradata2, pinfos->extradata3, 0, 0);

					if(finderRecvPort != 0)
					{
						ep.sendto(pBundle, finderRecvPort, pChannel->addr().ip);
						Network::Bundle::ObjPool().reclaimObject(pBundle);
					}
					else
					{
						pChannel->send(pBundle);
					}
				}

				++iter;
			}
			else
			{
				WARNING_MSG(fmt::format("Machine::onQueryAllInterfaceInfos: {}[{}] invalid, erase {}.\n",
					COMPONENT_NAME_EX(pinfos->componentType),
					pinfos->cid,
					COMPONENT_NAME_EX(pinfos->componentType)));

				iter = components.erase(iter);

				if(islocal)
					SystemInfo::getSingleton().clear();
			}
		}
	}
}

//-------------------------------------------------------------------------------------
bool Machine::findBroadcastInterface()
{

	std::map<u_int32_t, std::string> interfaces;
	Network::BundleBroadcast bhandler(networkInterface_, KBE_PORT_BROADCAST_DISCOVERY);

	if (!bhandler.epListen().getInterfaces(interfaces))
	{
		ERROR_MSG("Machine::findBroadcastInterface: Failed to discover network interfaces\n");
		return false;
	}

	uint8 data = 1;
	bhandler << data;
	if (!bhandler.broadcast(KBE_PORT_BROADCAST_DISCOVERY))
	{
		ERROR_MSG(fmt::format("Machine::findBroadcastInterface:Failed to send broadcast discovery message. error:{}\n",
			kbe_strerror()));
		return false;
	}
	
	sockaddr_in	sin;

	if(bhandler.receive(NULL, &sin))
	{
		INFO_MSG(fmt::format("Machine::findBroadcastInterface:Machine::findBroadcastInterface: Broadcast discovery receipt from {}.\n",
					inet_ntoa((struct in_addr&)sin.sin_addr.s_addr)));

		std::map< u_int32_t, std::string >::iterator iter;

		iter = interfaces.find( (u_int32_t &)sin.sin_addr.s_addr );
		if (iter != interfaces.end())
		{
			INFO_MSG(fmt::format("Machine::findBroadcastInterface: Confirmed {} ({}) as default broadcast route interface.\n",
				inet_ntoa((struct in_addr&)sin.sin_addr.s_addr),
				iter->second.c_str()));

			broadcastAddr_ = sin.sin_addr.s_addr;
			return true;
		}
	}
	
	std::string sinterface = "\t[";
	std::map< u_int32_t, std::string >::iterator iter = interfaces.begin();
	for(; iter != interfaces.end(); ++iter)
	{
		sinterface += inet_ntoa((struct in_addr&)iter->first);
		sinterface += ", ";
	}

	sinterface += "]";

	ERROR_MSG(fmt::format("Machine::findBroadcastInterface: Broadcast discovery [{}] "
		"not a valid interface. available interfaces:{}\n",
		inet_ntoa((struct in_addr&)sin.sin_addr.s_addr), sinterface.c_str()));

	return false;
}

//-------------------------------------------------------------------------------------
bool Machine::initNetwork()
{
	epBroadcast_.socket(SOCK_DGRAM);
	ep_.socket(SOCK_DGRAM);
	epLocal_.socket(SOCK_DGRAM); 

	Network::Address address;
	address.ip = 0;
	address.port = 0;

	if (broadcastAddr_ == 0 && !this->findBroadcastInterface())
	{
		ERROR_MSG("Machine::initNetwork: Failed to determine default broadcast interface. "
				"Make sure that your broadcast route is set correctly. "
				"e.g. /sbin/ip route add broadcast 255.255.255.255 dev eth0\n" );

		return false;
	}

	if (!ep_.good() ||
		 ep_.bind(htons(KBE_MACHINE_BRAODCAST_SEND_PORT), broadcastAddr_) == -1)
	{
		ERROR_MSG(fmt::format("Machine::initNetwork: Failed to bind socket to '{}:{}'. {}.\n",
							inet_ntoa((struct in_addr &)broadcastAddr_),
							(KBE_MACHINE_BRAODCAST_SEND_PORT),
							kbe_strerror()));

		return false;
	}

	address.ip = broadcastAddr_;
	address.port = htons(KBE_MACHINE_BRAODCAST_SEND_PORT);
	ep_.setbroadcast( true );
	ep_.setnonblocking(true);
	ep_.addr(address);
	pEPPacketReceiver_ = new Network::UDPPacketReceiver(ep_, this->networkInterface());

	if(!this->dispatcher().registerReadFileDescriptor(ep_, pEPPacketReceiver_))
	{
		ERROR_MSG("Machine::initNetwork: registerReadFileDescriptor ep is failed!\n");
		return false;
	}
	
#if KBE_PLATFORM == PLATFORM_WIN32
	u_int32_t baddr = htonl(INADDR_ANY);
#else
	u_int32_t baddr = Network::BROADCAST;
#endif

	if (!epBroadcast_.good() ||
		epBroadcast_.bind(htons(KBE_MACHINE_BRAODCAST_SEND_PORT), baddr) == -1)
	{
		ERROR_MSG(fmt::format("Machine::initNetwork: Failed to bind socket to '{}:{}'. {}.\n",
							inet_ntoa((struct in_addr &)baddr),
							(KBE_MACHINE_BRAODCAST_SEND_PORT),
							kbe_strerror()));

#if KBE_PLATFORM != PLATFORM_WIN32
		return false;
#endif
	}
	else
	{
		address.ip = baddr;
		address.port = htons(KBE_MACHINE_BRAODCAST_SEND_PORT);
		epBroadcast_.setnonblocking(true);
		epBroadcast_.addr(address);
		pEBPacketReceiver_ = new Network::UDPPacketReceiver(epBroadcast_, this->networkInterface());
	
		if(!this->dispatcher().registerReadFileDescriptor(epBroadcast_, pEBPacketReceiver_))
		{
			ERROR_MSG("Machine::initNetwork: registerReadFileDescriptor epBroadcast is failed!\n");
			return false;
		}

	}

	if (!epLocal_.good() ||
		 epLocal_.bind(htons(KBE_MACHINE_BRAODCAST_SEND_PORT), Network::LOCALHOST) == -1)
	{
		ERROR_MSG(fmt::format("Machine::initNetwork: Failed to bind socket to (lo). {}.\n",
							kbe_strerror()));

		return false;
	}

	address.ip = Network::LOCALHOST;
	address.port = htons(KBE_MACHINE_BRAODCAST_SEND_PORT);
	epLocal_.setnonblocking(true);
	epLocal_.addr(address);
	pEPLocalPacketReceiver_ = new Network::UDPPacketReceiver(epLocal_, this->networkInterface());

	if(!this->dispatcher().registerReadFileDescriptor(epLocal_, pEPLocalPacketReceiver_))
	{
		ERROR_MSG("Machine::initNetwork: registerReadFileDescriptor epLocal is failed!\n");
		return false;
	}

	INFO_MSG(fmt::format("Machine::initNetwork: bind broadcast successfully! addr:{}\n", ep_.addr().c_str()));
	return true;
}

//-------------------------------------------------------------------------------------
bool Machine::run()
{
	bool ret = true;

	while(!this->dispatcher().hasBreakProcessing())
	{
		threadPool_.onMainThreadTick();
		this->dispatcher().processOnce(false);
		networkInterface().processChannels(&MachineInterface::messageHandlers);
		KBEngine::sleep(100);
	};

	return ret;
}

//-------------------------------------------------------------------------------------
void Machine::handleTimeout(TimerHandle handle, void * arg)
{
	ServerApp::handleTimeout(handle, arg);
}

//-------------------------------------------------------------------------------------
bool Machine::initializeBegin()
{
	if(!initNetwork())
		return false;

	return true;
}

//-------------------------------------------------------------------------------------
bool Machine::inInitialize()
{
	return true;
}

//-------------------------------------------------------------------------------------
bool Machine::initializeEnd()
{
	pActiveTimerHandle_->cancel(); // machine????????????????????????????????
	return true;
}

//-------------------------------------------------------------------------------------
void Machine::finalise()
{
	ServerApp::finalise();
}

//-------------------------------------------------------------------------------------
void Machine::startserver(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	int32 uid = 0;
	COMPONENT_TYPE componentType;

	Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();
	bool success = true;

	uint16 finderRecvPort = 0;

	s >> uid;
	s >> componentType;

	if(s.length() > 0)
	{
		s >> finderRecvPort;
	}

	INFO_MSG(fmt::format("Machine::startserver: uid={}, [{}], addr={}\n", 
		uid,  COMPONENT_NAME_EX(componentType), pChannel->c_str()));
	
	if(ComponentName2ComponentType(COMPONENT_NAME_EX(componentType)) == UNKNOWN_COMPONENT_TYPE)
		return;

#if KBE_PLATFORM == PLATFORM_WIN32
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	std::string str = Resmgr::getSingleton().getEnv().bin_path;
	str += COMPONENT_NAME_EX(componentType);
	str += ".exe";

	wchar_t* szCmdline = KBEngine::strutil::char2wchar(str.c_str());
	wchar_t* currdir = KBEngine::strutil::char2wchar(Resmgr::getSingleton().getEnv().bin_path.c_str());

	ZeroMemory( &si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory( &pi, sizeof(pi));

	if(!CreateProcess( NULL,   // No module name (use command line)
		szCmdline,      // Command line
		NULL,           // Process handle not inheritable
		NULL,           // Thread handle not inheritable
		FALSE,          // Set handle inheritance to FALSE
		CREATE_NEW_CONSOLE,    // No creation flags
		NULL,           // Use parent's environment block
		currdir,        // Use parent's starting directory
		&si,            // Pointer to STARTUPINFO structure
		&pi )           // Pointer to PROCESS_INFORMATION structure
	)
	{
		ERROR_MSG(fmt::format("Machine::startserver:CreateProcess failed ({}).\n",
			GetLastError()));

		success = false;
	}
	
	free(szCmdline);
	free(currdir);
#else
#endif
	
	(*pBundle) << success;

	if(finderRecvPort != 0)
	{
		Network::EndPoint ep;
		ep.socket(SOCK_DGRAM);

		if (!ep.good())
		{
			ERROR_MSG("Machine::startserver: Failed to create socket.\n");
			return;
		}
	
		ep.sendto(pBundle, htons(finderRecvPort), pChannel->addr().ip);
		Network::Bundle::ObjPool().reclaimObject(pBundle);
	}
	else
	{
		pChannel->send(pBundle);
	}
}

//-------------------------------------------------------------------------------------
void Machine::stopserver(Network::Channel* pChannel, KBEngine::MemoryStream& s)
{
	int32 uid = 0;
	COMPONENT_TYPE componentType;
	Network::Bundle* pBundle = Network::Bundle::ObjPool().createObject();
	bool success = true;

	uint16 finderRecvPort = 0;

	s >> uid;
	s >> componentType;
	
	if(s.length() > 0)
	{
		s >> finderRecvPort;
	}

	INFO_MSG(fmt::format("Machine::stopserver: request uid={}, [{}], addr={}\n", 
		uid,  COMPONENT_NAME_EX(componentType), pChannel->c_str()));

	if(ComponentName2ComponentType(COMPONENT_NAME_EX(componentType)) == UNKNOWN_COMPONENT_TYPE)
	{
		ERROR_MSG(fmt::format("Machine::stopserver: component({}) is error!", 
			(int)ComponentName2ComponentType(COMPONENT_NAME_EX(componentType)), 
			pChannel->c_str()));

		return;
	}

	Components::COMPONENTS& components = Components::getSingleton().getComponents(componentType);

	if(components.size() > 0)
	{
		Components::COMPONENTS::iterator iter = components.begin();

		for(; iter != components.end(); )
		{
			Components::ComponentInfos* cinfos = &(*iter);

			if(cinfos->uid != uid)
			{
				iter++;
				continue;
			}

			if(componentType != cinfos->componentType)
			{
				iter++;
				continue;
			}

			if(((*iter).flags & COMPONENT_FLAG_SHUTTINGDOWN) > 0)
			{
				iter++;
				continue;
			}

			INFO_MSG(fmt::format("--> stop {}({}), addr={}\n", 
				(*iter).cid, COMPONENT_NAME[componentType], (cinfos->pIntAddr != NULL ? cinfos->pIntAddr->c_str() : "unknown")));

			bool usable = checkComponentUsable(&(*iter), false, false);
		
			if(!usable)
			{
				iter = components.erase(iter);
				continue;
			}


			Network::Bundle closebundle;
			if(componentType != BOTS_TYPE)
			{
				COMMON_NETWORK_MESSAGE(componentType, closebundle, reqCloseServer);
			}
			else
			{
				closebundle.newMessage(BotsInterface::reqCloseServer);
			}

			Network::EndPoint ep1;
			ep1.socket(SOCK_STREAM);

			if (!ep1.good())
			{
				ERROR_MSG("Machine::stopserver: Failed to create socket.\n");
				success = false;
				break;
			}
		
			if(ep1.connect((*iter).pIntAddr.get()->port, (*iter).pIntAddr.get()->ip) == -1)
			{
				ERROR_MSG(fmt::format("Machine::stopserver: connect server is error({})!\n", kbe_strerror()));
				success = false;
				break;
			}

			ep1.setnonblocking(false);
			ep1.send(&closebundle);

			Network::TCPPacket recvpacket;
			recvpacket.resize(255);

			fd_set	fds;
			struct timeval tv = { 0, 1000000 }; // 1000ms

			FD_ZERO( &fds );
			FD_SET((int)ep1, &fds);

			int selgot = select(ep1+1, &fds, NULL, NULL, &tv);
			if(selgot == 0)
			{
				// ????, ????????????
				ERROR_MSG(fmt::format("--> stop {}({}), addr={}, timeout!\n", 
					(*iter).cid, COMPONENT_NAME[componentType], (cinfos->pIntAddr != NULL ? 
					cinfos->pIntAddr->c_str() : "unknown")));

				iter++;
				continue;
			}
			else if(selgot == -1)
			{
				WARNING_MSG(fmt::format("--> stop {}({}), addr={}, recv_len == -1!\n", 
					(*iter).cid, COMPONENT_NAME[componentType], (cinfos->pIntAddr != NULL ? 
					cinfos->pIntAddr->c_str() : "unknown")));

				iter++;
				continue;
			}

			(*iter).flags |= COMPONENT_FLAG_SHUTTINGDOWN;

			int len = ep1.recv(recvpacket.data(), 1);
			if(len != 1)
			{
				ERROR_MSG(fmt::format("--> stop {}({}), addr={}, recv_len != 1!\n", 
					(*iter).cid, COMPONENT_NAME[componentType], (cinfos->pIntAddr != NULL ? 
					cinfos->pIntAddr->c_str() : "unknown")));

				success = false;
				break;
			}

			recvpacket >> success;
			iter++;
		}
	}
	else
	{
		INFO_MSG(fmt::format("Machine::stopserver: uid={}, {} size is 0, addr={}\n", 
			uid,  COMPONENT_NAME_EX(componentType), pChannel->c_str()));
	}

	(*pBundle) << success;

	if(finderRecvPort != 0)
	{
		Network::EndPoint ep;
		ep.socket(SOCK_DGRAM);

		if (!ep.good())
		{
			ERROR_MSG("Machine::stopserver: Failed to create socket.\n");
			return;
		}
	
		ep.sendto(pBundle, finderRecvPort, pChannel->addr().ip);
		Network::Bundle::ObjPool().reclaimObject(pBundle);
	}
	else
	{
		pChannel->send(pBundle);
	}
}


//-------------------------------------------------------------------------------------

}
