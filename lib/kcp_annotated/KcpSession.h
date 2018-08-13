#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <string>
#include "ikcp.h"
#include "Buf.h"


class KcpSession
{
public:
	enum StateE { kDisconnected, kConnecting, kConnected, kDisconnecting };
	enum DataTypeE { kUnreliable = 88, kReliable };
	enum PktTypeE { kSyn = 66, kAck, kPsh, kFin };

	typedef std::function<void(const void* data, int len)> OutputFunction;
	typedef std::function<IUINT32()> CurrentTimeCallBack;

public:
	KcpSession(const OutputFunction& outputFunc, const CurrentTimeCallBack& currentTimeCb)
		:
		conv_(0),
		outputFunc_(outputFunc),
		curTimeCb_(currentTimeCb),
		kcpcb_(nullptr),
		kcpConnState_(kConnecting)
	{}

	~KcpSession() { ikcp_release(kcpcb_); }

	bool IsKcpConnected() const { return kcpConnState_ == kConnected; }
	bool IsKcpDisconnected() const { return kcpConnState_ == kDisconnected; }

	// returns below zero for error
	int Send(const void* data, int len, DataTypeE dataType = kReliable)
	{
		outputBuf_.retrieveAll();
		assert(dataType == kReliable || dataType == kUnreliable);
		if (dataType == kUnreliable)
		{
			outputBuf_.appendInt8(kUnreliable);
			outputBuf_.append(data, len);
			outputFunc_(outputBuf_.peek(), outputBuf_.readableBytes());
			return 0;
		}
		else if (dataType == kReliable)
		{
			if (!IsKcpConnected())
			{
				SendSyn();
				//sndQueueBeforeConned_.push(std::string(static_cast<const char*>(data), len));
				return 0;
			}
			else if (IsKcpConnected())
			{
				//while (sndQueueBeforeConned_.size() > 0)
				//{
				//	std::string msg = sndQueueBeforeConned_.front();
				//	int sendRet = ikcp_send(kcpcb_, msg.c_str(), msg.size());
				//	if (sendRet < 0)
				//		return sendRet; // ikcp_send err
				//	else
				//		ikcp_update(kcpcb_, curTimeCb_());
				//	sndQueueBeforeConned_.pop();
				//}

				int result = ikcp_send(
					kcpcb_,
					static_cast<const char*>(data),
					len);
				if (result < 0)
					return result; // ikcp_send err
				else
					ikcp_update(kcpcb_, curTimeCb_());
				return 0;
			}
		}
		return 0;
	}

	int Recv(char* data, size_t len)
	{
		inputBuf_.retrieveAll();
		inputBuf_.append(data, len);
		auto dataType = inputBuf_.readInt8();
		if (dataType == kUnreliable)
		{
			auto readableBytes = inputBuf_.readableBytes();
			memcpy(data, inputBuf_.peek(), readableBytes);
			return readableBytes;
		}
		else if (dataType == kReliable)
		{
			auto pktType = inputBuf_.readInt8();
			if (pktType == kSyn)
			{
				if (!IsKcpConnected())
				{
					SetKcpConnectState(kConnected);
					InitKcp(GetNewConv());
				}
				SendAckAndConv();
				return 0;
			}
			else if (pktType == kAck)
			{
				if (!IsKcpConnected())
				{
					SetKcpConnectState(kConnected);
					InitKcp(inputBuf_.readInt32());
				}
				return 0;
			}
			else if (pktType == kPsh)
			{
				if (IsKcpConnected())
				{
					auto readableBytes = inputBuf_.readableBytes();
					int result = ikcp_input(kcpcb_, inputBuf_.peek(), readableBytes);
					if (result == 0)
						return KcpRecv(data); // if err, -1, -2, -3
					else // if (result < 0)
						return result - 3; // ikcp_input err, -4, -5, -6
				}
				else // !IsKcpConnected
				{
					//return -7; // pktType == kPsh, but kcp not connected err
					return 0;
				}
			}
			else
			{
				return -8; // pktType err
			}
		}
		else
		{
			return -9; // dataType err
		}
	}


private:

	void SendSyn()
	{
		outputBuf_.appendInt8(kReliable);
		outputBuf_.appendInt8(kSyn);
		outputFunc_(outputBuf_.peek(), outputBuf_.readableBytes());
		outputBuf_.retrieveAll();
	}

	void SendAckAndConv()
	{
		outputBuf_.appendInt8(kReliable);
		outputBuf_.appendInt8(kAck);
		outputBuf_.appendInt32(conv_);
		outputFunc_(outputBuf_.peek(), outputBuf_.readableBytes());
		outputBuf_.retrieveAll();
	}

	void InitKcp(IUINT32 conv)
	{
		conv_ = conv;
		kcpcb_ = ikcp_create(conv, this);
		ikcp_wndsize(kcpcb_, 128, 128);
		ikcp_nodelay(kcpcb_, 1, 5, 1, 1); // 设置成1次ACK跨越直接重传, 这样反应速度会更快. 内部时钟5毫秒.
		kcpcb_->rx_minrto = 5;
		kcpcb_->output = KcpSession::KcpOutputFuncRaw;
	}

	IUINT32 GetNewConv()
	{
		static IUINT32 newConv = 666;
		return newConv++;
	}

	void SetKcpConnectState(StateE s) { kcpConnState_ = s; }

	int KcpRecv(char* userBuffer)
	{
		int msgLen = ikcp_peeksize(kcpcb_);
		if (msgLen <= 0)
		{
			return 0;
		}
		return ikcp_recv(kcpcb_, userBuffer, msgLen);
	}

	static int KcpOutputFuncRaw(const char* data, int len, IKCPCB* kcp, void* user)
	{
		(void)kcp;
		auto thisPtr = reinterpret_cast<KcpSession *>(user);
		assert(thisPtr->outputFunc_ != nullptr);

		thisPtr->outputBuf_.appendInt8(kReliable);
		thisPtr->outputBuf_.appendInt8(kPsh);
		thisPtr->outputBuf_.append(data, len);
		thisPtr->outputFunc_(thisPtr->outputBuf_.peek(), thisPtr->outputBuf_.readableBytes());
		thisPtr->outputBuf_.retrieveAll();

		return 0;
	}

private:
	ikcpcb* kcpcb_;
	OutputFunction outputFunc_;
	StateE kcpConnState_;
	Buf outputBuf_;
	Buf inputBuf_;
	CurrentTimeCallBack curTimeCb_;
	IUINT32 conv_;
	std::queue<std::string> sndQueueBeforeConned_;
};
typedef std::shared_ptr<KcpSession> KcpSessionPtr;