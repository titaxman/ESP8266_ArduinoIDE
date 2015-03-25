/****
 * Sming Framework Project - Open Source framework for high efficiency native ESP8266 development.
 * Created 2015 by Skurydin Alexey
 * http://github.com/anakod/Sming
 * All files of the Sming Core are provided under the LGPL v3 license.
 ****/

#include "TcpConnection.h"

#include "../../SmingCore/DataSourceStream.h"
#include "NetUtils.h"
#include "../Wiring/WString.h"
#include "../Wiring/IPAddress.h"

TcpConnection::TcpConnection(bool autoDestruct) : autoSelfDestruct(autoDestruct), sleep(0), canSend(true), timeOut(70)
{
	initialize(tcp_new());
}

TcpConnection::TcpConnection(tcp_pcb* connection, bool autoDestruct) : autoSelfDestruct(autoDestruct), sleep(0), canSend(true), timeOut(70)
{
	initialize(connection);
}

TcpConnection::~TcpConnection()
{
	close();

	debugf("~TCP connection");
}

bool TcpConnection::connect(const char* server, int port)
{
	if (tcp == NULL)
		initialize(tcp_new());

	ip_addr_t addr;

	debugf("connect to: %s", server);
	canSend = false; // Wait for connection
	DnsLookup *look = new DnsLookup { this, port };
	err_t dnslook = dns_gethostbyname(server, &addr, staticDnsResponse, look);
	if (dnslook != ERR_OK)
	{
		if (dnslook == ERR_INPROGRESS)
			return true;
		else
		{
			delete look;
			return false;
		}
	}
	delete look;

	return connect(addr, port);
}

bool TcpConnection::connect(IPAddress addr, uint16_t port)
{
	NetUtils::FixNetworkRouting();
	err_t res = tcp_connect(tcp, addr, port, staticOnConnected);
	debugf("TcpConnection::connect port: %d, %d", port, res);
	return res == ERR_OK;
}

void TcpConnection::setTimeOut(uint16_t waitTimeOut)
{
	timeOut = waitTimeOut;
}

err_t TcpConnection::onReceive(pbuf *buf)
{
	if (buf == NULL)
		debugf("TCP received: (null)");
	else
		debugf("TCP received: %d bytes", buf->tot_len);

	if (buf != NULL)
		onReadyToSendData(eTCE_Received);
	//else
	//	canSend = false;

	return ERR_OK;
}

err_t TcpConnection::onSent(uint16_t len)
{
	debugf("TCP sent: %d", len);

	//debugf("%d %d", tcp->state, tcp->flags); // WRONG!
	if (len >= 0 && tcp != NULL && canSend)
		onReadyToSendData(eTCE_Sent);

	return ERR_OK;
}

err_t TcpConnection::onPoll()
{
	if (sleep >= timeOut && timeOut != USHRT_MAX)
	{
		debugf("TCP connection closed by timeout");

		close();
		return ERR_OK;
	}

	if (tcp != NULL && canSend) //(tcp->state >= SYN_SENT && tcp->state <= ESTABLISHED))
		onReadyToSendData(eTCE_Poll);

	return ERR_OK;
}

err_t TcpConnection::onConnected(err_t err)
{
	if (err != ERR_OK)
		debugf("TCP connected error status: %d", err);
	else
		debugf("TCP connected");

	canSend = true;
	if (err == ERR_OK)
		onReadyToSendData(eTCE_Connected);
	else
		close();

	return ERR_OK;
}

void TcpConnection::onError(err_t err)
{
	debugf("TCP connection error: %d", err);
}

void TcpConnection::onReadyToSendData(TcpConnectionEvent sourceEvent)
{
	debugf("onReadyToSendData: %d", sourceEvent);
}

int TcpConnection::writeString(const char* data, uint8_t apiflags /* = 0*/)
{
	return write(data, os_strlen(data), apiflags);
}

int TcpConnection::write(const char* data, int len, uint8_t apiflags /* = 0*/)
{
   int original = len;
   err_t err;
   do
   {
	 err = tcp_write(tcp, data, len, apiflags);
	 if (err == ERR_MEM)
	 {
	   if ((tcp_sndbuf(tcp) == 0) || (tcp_sndqueuelen(tcp) >= TCP_SND_QUEUELEN)) {
		 /* no need to try smaller sizes */
		 len = 1;
	   } else {
		 len /= 2;
	   }
	 }
   } while ((err == ERR_MEM) && (len > 1));

   if (err == ERR_OK)
   {
		debugf("TCP connection send: %d (%d)", len, original);
		return len;
   } else {
		debugf("TCP connection failed with err %d (\"%s\")", err, lwip_strerr(err));
		return -1;
   }
}

int TcpConnection::write(IDataSourceStream* stream)
{
	// Send data from DataStream
	bool repeat;
	bool space;
	int total = 0;
	do
	{
		space = (tcp_sndqueuelen(tcp) < TCP_SND_QUEUELEN);
		if (!space)
		{
			debugf("WAIT FOR FREE SPACE");
			//connection.flush();
			break; // don't try to send buffers if no free space available
		}

		char* pointer;

		// Join small fragments
		int curPart = 0;
		int pushCount = 0;
		do
		{
			if (pushCount > 25) break;
			pushCount++;
			int available = stream->getDataPointer(&pointer);
			if (available <= 0) continue;
			int len = min(available, 4096);
			int written = write(pointer, len, TCP_WRITE_FLAG_COPY | TCP_WRITE_FLAG_MORE);
			curPart += written;
			total += written;
			stream->seek(max(written, 0));
			repeat = len > 0 && written == len && !stream->isFinished();
		} while (repeat && curPart < NETWORK_SEND_BUFFER_SIZE);

		space = (tcp_sndqueuelen(tcp) < TCP_SND_QUEUELEN);// && tcp_sndbuf(tcp) >= FILE_STREAM_BUFFER_SIZE;
	} while (repeat && space);

	flush();
	return total;
}

void TcpConnection::close()
{
	if (tcp == NULL) return;
	debugf("TCP connection closing");

	tcp_arg(tcp, NULL); // reset pointer to close connection on next callback
	tcp = NULL;
}

void TcpConnection::initialize(tcp_pcb* pcb)
{
	tcp = pcb;
	sleep = 0;
	canSend = true;
	tcp_arg(tcp, (void*)this);
	tcp_sent(tcp, staticOnSent);
	tcp_recv(tcp, staticOnReceive);
	tcp_err(tcp, staticOnError);
	tcp_poll(tcp, staticOnPoll, 4);

	#ifdef NETWORK_DEBUG
	debugf("+TCP connection");
	#endif
}

void TcpConnection::closeTcpConnection(tcp_pcb *tpcb)
{
	if (tpcb == NULL) return;

	debugf("-TCP connection");

	tcp_arg(tpcb, NULL);
	tcp_sent(tpcb, NULL);
	tcp_recv(tpcb, NULL);
	tcp_err(tpcb, NULL);
	tcp_poll(tpcb, NULL, 0);
	tcp_accept(tpcb, NULL);

	auto err = tcp_close(tpcb);
	if (err != ERR_OK)
	{
		debugf("TCP CAN'T CLOSE CONNECTION");
		/* error closing, try again later in poll */
		tcp_poll(tpcb, staticOnPoll, 4);
	}
}

void TcpConnection::flush()
{
	if (tcp->state == ESTABLISHED)
		tcp_output(tcp);
}

err_t TcpConnection::staticOnConnected(void *arg, tcp_pcb *tcp, err_t err)
{
	debugf("OnConnected %X", (int)arg);
	TcpConnection* con = (TcpConnection*)arg;
	if (con == NULL)
	{
		debugf("OnConnected ABORT");
		//closeTcpConnection(tcp);
		tcp_abort(tcp);
		return ERR_ABRT;
	}

	err_t res = con->onConnected(err);
	con->checkSelfFree();
	//debugf("<staticOnConnected");
	return res;
}

err_t TcpConnection::staticOnReceive(void *arg, tcp_pcb *tcp, pbuf *p, err_t err)
{
	TcpConnection* con = (TcpConnection*)arg;
	err_t ret_err;
	//Serial.println("echo_recv!");

	if (con == NULL)
	{
		if (p != NULL)
		{
		  /* Inform TCP that we have taken the data. */
		  tcp_recved(tcp, p->tot_len);
		  pbuf_free(p);
		}
		closeTcpConnection(tcp);
		return ERR_OK;
	}
	else
		con->sleep = 0;

	if (err != ERR_OK /*&& err != ERR_CLSD && err != ERR_RST*/)
	{
		debugf("Received ERROR %d", err);
		/* exit and free resources, for unkown reason */
		if (p != NULL)
		{
		  /* Inform TCP that we have taken the data. */
		  //tcp_recved(tcp, p->tot_len);
		  pbuf_free(p);
		}
		closeTcpConnection(tcp); // ??
		con->tcp = NULL;
		con->onError(err);
		//con->close();
		return err;
	}

	//if (tcp != NULL && tcp->state == ESTABLISHED) // If active
	/* We have taken the data. */
	if (p != NULL)
		tcp_recved(tcp, p->tot_len);

	err_t res = con->onReceive(p);

	if (p != NULL)
		pbuf_free(p);
	else
	{
		con->close();
		closeTcpConnection(tcp);
	}

	con->checkSelfFree();
	//debugf("<staticOnReceive");
	return res;
}

err_t TcpConnection::staticOnSent(void *arg, tcp_pcb *tcp, uint16_t len)
{
	TcpConnection* con = (TcpConnection*)arg;

	if (con == NULL)
		return ERR_OK;
	else
		con->sleep = 0;

	err_t res = con->onSent(len);
	con->checkSelfFree();
	//debugf("<staticOnSent");
	return res;
}

err_t TcpConnection::staticOnPoll(void *arg, tcp_pcb *tcp)
{
	TcpConnection* con = (TcpConnection*)arg;

	if (con == NULL)
	{
		closeTcpConnection(tcp);
		return ERR_OK;
	}

	//if (tcp->state != ESTABLISHED)
	//	return ERR_OK;

	con->sleep++;
	err_t res = con->onPoll();
	con->checkSelfFree();
	//debugf("<staticOnPoll");
	return res;
}

void TcpConnection::staticOnError(void *arg, err_t err)
{
	TcpConnection* con = (TcpConnection*)arg;
	if (con == NULL) return;

	con->tcp = NULL; // IMPORTANT. No available connection after error!
	con->onError(err);
	con->checkSelfFree();
	//debugf("<staticOnError");
}

void TcpConnection::staticDnsResponse(const char *name, ip_addr_t *ipaddr, void *arg)
{
	DnsLookup* dlook = (DnsLookup*)arg;
	if (dlook == NULL) return;

	if (ipaddr != NULL)
	{
		IPAddress ip = *ipaddr;
		debugf("DNS record found: %s = %d.%d.%d.%d",
				name, ip[0], ip[1], ip[2], ip[3]);

		dlook->con->connect(ip, dlook->port);
	}
	else
	{
		#ifdef NETWORK_DEBUG
		debugf("DNS record _not_ found: %s", name);
		#endif

		closeTcpConnection(dlook->con->tcp);
		dlook->con->tcp = NULL;
		dlook->con->close();
	}

	delete dlook;
}
