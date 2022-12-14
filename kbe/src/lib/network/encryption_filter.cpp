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

#include "helper/profile.h"
#include "encryption_filter.h"
#include "helper/debug_helper.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/channel.h"
#include "network/network_interface.h"
#include "network/packet_receiver.h"
#include "network/packet_sender.h"

namespace KBEngine { 
namespace Network
{

//-------------------------------------------------------------------------------------
BlowfishFilter::BlowfishFilter(const Key & key):
KBEBlowfish(key),
pPacket_(NULL),
packetLen_(0),
padSize_(0)
{
}

//-------------------------------------------------------------------------------------
BlowfishFilter::BlowfishFilter():
KBEBlowfish(),
pPacket_(NULL),
packetLen_(0),
padSize_(0)
{
}

//-------------------------------------------------------------------------------------
BlowfishFilter::~BlowfishFilter()
{
	if(pPacket_)
	{
		RECLAIM_PACKET(pPacket_->isTCPPacket(), pPacket_);
		pPacket_ = NULL;
	}
}

//-------------------------------------------------------------------------------------
Reason BlowfishFilter::send(Channel * pChannel, PacketSender& sender, Packet * pPacket)
{
	if(!pPacket->encrypted())
	{
		AUTO_SCOPED_PROFILE("encryptSend")
		
		if (!isGood_)
		{
			WARNING_MSG(fmt::format("BlowfishFilter::send: "
				"Dropping packet to {} due to invalid filter\n",
				pChannel->addr().c_str()));

			return REASON_GENERAL_NETWORK;
		}

		Packet * pOutPacket = NULL;
		MALLOC_PACKET(pOutPacket, pPacket->isTCPPacket());

		PacketLength oldlen = pPacket->length();
		pOutPacket->wpos(PACKET_LENGTH_SIZE + 1);
		encrypt(pPacket, pOutPacket);

		PacketLength packetLen = pPacket->length() + 1;
		uint8 padSize = pPacket->length() - oldlen;
		size_t oldwpos =  pOutPacket->wpos();
		pOutPacket->wpos(0);

		(*pOutPacket) << packetLen;
		(*pOutPacket) << padSize;

		pOutPacket->wpos(oldwpos);
		pPacket->swap(*(static_cast<KBEngine::MemoryStream*>(pOutPacket)));
		RECLAIM_PACKET(pPacket->isTCPPacket(), pOutPacket);

		/*
		if(Network::g_trace_packet > 0)
		{
			DEBUG_MSG(fmt::format("BlowfishFilter::send: packetLen={}, padSize={}\n",
				packetLen, (int)padSize));
		}
		*/
	}

	return sender.processFilterPacket(pChannel, pPacket);
}

//-------------------------------------------------------------------------------------
Reason BlowfishFilter::recv(Channel * pChannel, PacketReceiver & receiver, Packet * pPacket)
{
	while(pPacket || pPacket_)
	{
		AUTO_SCOPED_PROFILE("encryptRecv")

		if (!isGood_)
		{
			WARNING_MSG(fmt::format("BlowfishFilter::recv: "
				"Dropping packet to {} due to invalid filter\n",
				pChannel->addr().c_str()));

			return REASON_GENERAL_NETWORK;
		}

		if(pPacket_)
		{
			if(pPacket)
			{
				pPacket_->append(pPacket->data() + pPacket->rpos(), pPacket->length());
				RECLAIM_PACKET(pPacket->isTCPPacket(), pPacket);
			}

			pPacket = pPacket_;
		}

		if(packetLen_ <= 0)
		{
			// ????????????????????????????, ??????????????????????????????????????
			if(pPacket->length() >= (PACKET_LENGTH_SIZE + 1 + BLOCK_SIZE))
			{
				(*pPacket) >> packetLen_;
				(*pPacket) >> padSize_;
				
				packetLen_ -= 1;

				// ???????????????????????????? ????????????????????????????????????????????????
				if(pPacket->length() > packetLen_)
				{
					MALLOC_PACKET(pPacket_, pPacket->isTCPPacket());
					pPacket_->append(pPacket->data() + pPacket->rpos() + packetLen_, pPacket->wpos() - (packetLen_ + pPacket->rpos()));
					pPacket->wpos(pPacket->rpos() + packetLen_);
				}
				else if(pPacket->length() == packetLen_)
				{
					if(pPacket_ != NULL)
						pPacket_ = NULL;
				}
				else
				{
					if(pPacket_ == NULL)
						pPacket_ = pPacket;

					return receiver.processFilteredPacket(pChannel, NULL);
				}
			}
			else
			{
				if(pPacket_ == NULL)
					pPacket_ = pPacket;

				return receiver.processFilteredPacket(pChannel, NULL);
			}
		}
		else
		{
			// ????????????????????????????????????????????????
			if(pPacket->length() > packetLen_)
			{
				MALLOC_PACKET(pPacket_, pPacket->isTCPPacket());
				pPacket_->append(pPacket->data() + pPacket->rpos() + packetLen_, pPacket->wpos() - (packetLen_ + pPacket->rpos()));
				pPacket->wpos(pPacket->rpos() + packetLen_);
			}
			else if(pPacket->length() == packetLen_)
			{
				if(pPacket_ != NULL)
					pPacket_ = NULL;
			}
			else
			{
				if(pPacket_ == NULL)
					pPacket_ = pPacket;

				return receiver.processFilteredPacket(pChannel, NULL);
			}
		}

		decrypt(pPacket, pPacket);

		pPacket->wpos(pPacket->wpos() - padSize_);

		/*
		if(Network::g_trace_packet > 0)
		{
			DEBUG_MSG(fmt::format("BlowfishFilter::recv: packetLen={}, padSize={}\n",
				(packetLen_ + 1), (int)padSize_));
		}
		*/

		packetLen_ = 0;
		padSize_ = 0;

		Reason ret = receiver.processFilteredPacket(pChannel, pPacket);
		if(ret != REASON_SUCCESS)
		{
			if(pPacket_)
			{
				RECLAIM_PACKET(pPacket_->isTCPPacket(), pPacket);
				pPacket_ = NULL;
			}
			return ret;
		}

		pPacket = NULL;
	}

	return REASON_SUCCESS;
}

//-------------------------------------------------------------------------------------
void BlowfishFilter::encrypt(Packet * pInPacket, Packet * pOutPacket)
{
	// BlowFish ??????????????????8????????
	// ????8??????????0
	uint8 padSize = 0;

	if (pInPacket->length() % BLOCK_SIZE != 0)
	{
		// ????????????
		padSize = BLOCK_SIZE - (pInPacket->length() % BLOCK_SIZE);

		// ??pPacket????????????
		pInPacket->data_resize(pInPacket->size() + padSize);

		// ????0
		memset(pInPacket->data() + pInPacket->wpos(), 0, padSize);

		pInPacket->wpos(pInPacket->wpos() + padSize);
	}
	
	if(pInPacket != pOutPacket)
	{
		pOutPacket->data_resize(pInPacket->size() + pOutPacket->wpos());
		int size = KBEBlowfish::encrypt(pInPacket->data(), pOutPacket->data() + pOutPacket->wpos(),  pInPacket->wpos());
		pOutPacket->wpos(size + pOutPacket->wpos());
	}
	else
	{
		if(pInPacket->isTCPPacket())
			pOutPacket = TCPPacket::ObjPool().createObject();
		else
			pOutPacket = UDPPacket::ObjPool().createObject();

		pOutPacket->data_resize(pInPacket->size() + 1);

		int size = KBEBlowfish::encrypt(pInPacket->data(), pOutPacket->data() + pOutPacket->wpos(),  pInPacket->wpos());
		pOutPacket->wpos(size);

		pInPacket->swap(*(static_cast<KBEngine::MemoryStream*>(pOutPacket)));
		RECLAIM_PACKET(pInPacket->isTCPPacket(), pOutPacket);
	}

	pInPacket->encrypted(true);
}

//-------------------------------------------------------------------------------------
void BlowfishFilter::decrypt(Packet * pInPacket, Packet * pOutPacket)
{
	if(pInPacket != pOutPacket)
	{
		pOutPacket->data_resize(pInPacket->size());

		int size = KBEBlowfish::decrypt(pInPacket->data() + pInPacket->rpos(), 
			pOutPacket->data() + pOutPacket->rpos(),  
			pInPacket->wpos() - pInPacket->rpos());

		pOutPacket->wpos(size + pOutPacket->wpos());
	}
	else
	{
		KBEBlowfish::decrypt(pInPacket->data() + pInPacket->rpos(), pInPacket->data(),  
			pInPacket->wpos() - pInPacket->rpos());

		pInPacket->wpos(pInPacket->wpos() - pInPacket->rpos());
		pInPacket->rpos(0);
	}
}

//-------------------------------------------------------------------------------------

} 
}
