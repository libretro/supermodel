#ifndef INCLUDED_NETBOARD_H
#define INCLUDED_NETBOARD_H

#include "Types.h"
#include "CPU/Bus.h"
#include "OSD/Thread.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <thread>
#include "UDPSend.h"
#include "UDPReceive.h"

//#define NET_BUF_SIZE 32800 // 16384 not enough

class CNetBoard : public IBus
{
public:

	UINT8 Read8(UINT32 addr);
	UINT16 Read16(UINT32 addr);
	UINT32 Read32(UINT32 addr);

	void Write8(UINT32 addr, UINT8 data);
	void Write16(UINT32 addr, UINT16 data);
	void Write32(UINT32 addr, UINT32 data);

	void SaveState(CBlockFile *SaveState);
	void LoadState(CBlockFile *SaveState);

	bool RunFrame(void);
	void Reset(void);

	// Returns a reference to the 68K CPU context
	M68KCtx *GetM68K(void);
	bool IsAttached(void);
	bool CodeReady;

	bool Init(UINT8 *netRAMPtr, UINT8 *netBufferPtr);
	
	void GetGame(Game);

	CNetBoard(const Util::Config::Node &config);
	~CNetBoard(void);

private:
	// Config
	const Util::Config::Node &m_config;
	// 68K CPU
	M68KCtx		M68K;

	// Sound board memory
	UINT8		*netRAM;		// 128Kb RAM (passed in from parent object)
	UINT8		*netBuffer;		// 128kb (passed in from parent object)
	UINT8		*memoryPool;	// single allocated region for all net board RAM
	UINT8		*CommRAM;
	UINT8		*ioreg;
	UINT8		*ctrlrw;
	UINT8		*Buffer;
	UINT8		*RAM;
	UINT8		*ct;

	bool		m_attached;		// True if net board is attached
	UINT16		commbank;
	UINT16		recv_offset;
	UINT16		recv_size;
	UINT16		send_offset;
	UINT16		send_size;
	UINT8		slot;

	// netsock
	UINT16 port_in = 0;
	UINT16 port_out = 0;
	std::string addr_out = "";

	SMUDP::UDPSend udpSend;
	SMUDP::UDPReceive udpReceive;

	//game info
	Game Gameinfo;

	// only for some tests
	UINT8 *bank;
	UINT8 *bank2;
	UINT8 test_irq;

	std::thread interrupt5;
	bool int5;
};

void Net_SetCB(int(*Run68k)(int cycles), void(*Int68k)(int irq));

#endif	// INCLUDED_NETBOARD_H


