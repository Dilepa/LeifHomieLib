#include "HomieDevice.h"
#include "HomieNode.h"
void HomieLibDebugPrint(const char * szText);

#define csprintf(...) { char szTemp[256]; sprintf(szTemp,__VA_ARGS__ ); HomieLibDebugPrint(szTemp); }


#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#else
#include "WiFi.h"
#endif


static std::vector<HomieDebugPrintCallback> vecDebugPrint;

void HomieLibRegisterDebugPrintCallback(HomieDebugPrintCallback cb)
{
	vecDebugPrint.push_back(cb);

}

void HomieLibDebugPrint(const char * szText)
{
	for(size_t i=0;i<vecDebugPrint.size();i++)
	{
		vecDebugPrint[i](szText);
	}

}


HomieDevice * pToken=NULL;

bool AllowInitialPublishing(HomieDevice * pSource)
{
	if(pToken==pSource) return true;
	if(!pToken)
	{
		pToken=pSource;
		return true;
	}
	return false;
}

void FinishInitialPublishing(HomieDevice * pSource)
{
	if(pToken==pSource)
	{
		pToken=NULL;
	}
}




HomieDevice::HomieDevice()
{
}


void HomieDevice::Init()
{

	strTopic=String("homie/")+strID;
	strcpy(szWillTopic,String(strTopic+"/$state").c_str());

	if(!vecNode.size())
	{
		HomieNode * pNode=NewNode();

		pNode->strID="dummy";
		pNode->strFriendlyName="No Nodes";

/*
 	 	lots of mqtt:homie300:srvrm-lightsense:dummy#dummy triggered in the log

  		HomieProperty * pProp=pNode->NewProperty();
		pProp->strID="dummy";
		pProp->strFriendlyName="No Properties";
		pProp->bRetained=false;
		pProp->datatype=homieString;*/

	}

	for(size_t a=0;a<vecNode.size();a++)
	{
		vecNode[a]->Init();
	}


	IPAddress ip;
	ip.fromString(strMqttServerIP);
	//ip.fromString("172.22.22.99");
	mqtt.setServer(ip,1883);//1883
	mqtt.setCredentials(strMqttUserName.c_str(), strMqttPassword.c_str());

	mqtt.setWill(szWillTopic,2,true,"lost");

	mqtt.onConnect(std::bind(&HomieDevice::onConnect, this, std::placeholders::_1));
	mqtt.onDisconnect(std::bind(&HomieDevice::onDisconnect, this, std::placeholders::_1));
	mqtt.onMessage(std::bind(&HomieDevice::onMqttMessage, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5, std::placeholders::_6));

	bInitialized=true;
}

void HomieDevice::Quit()
{
	mqtt.publish(String(strTopic+"/$state").c_str(), 1, true, "disconnected");
	mqtt.disconnect(false);
	bInitialized=false;
}

bool HomieDevice::IsConnected()
{
	return mqtt.connected();
}


int iWiFiRSSI=0;

void HomieDevice::Loop()
{
	if(!bInitialized) return;

	if((int) (millis()-ulLastLoopSeconds)>=1000)
	{
		ulLastLoopSeconds+=1000;
		ulSecondCounter++;


		if(bRapidUpdateRSSI && (ulSecondCounter & 1))
		{
			int iWiFiRSSI_Current=WiFi.RSSI();
			if(iWiFiRSSI!=iWiFiRSSI_Current)
			{
				iWiFiRSSI=iWiFiRSSI_Current;

				mqtt.publish(String(strTopic+"/$stats/signal").c_str(), 2, true, String(iWiFiRSSI).c_str());
			}
		}

	}

	if(WiFi.status() != WL_CONNECTED) return;


	if(mqtt.connected())
	{

		DoInitialPublishing();

//		pubsubClient.loop();
		ulMqttReconnectCount=0;

		if(millis()-ulHomieStatsTimestamp>=30000)
		{
			ulHomieStatsTimestamp=millis();

			mqtt.publish(String(strTopic+"/$stats/uptime").c_str(), 2, true, String(ulSecondCounter).c_str());
			mqtt.publish(String(strTopic+"/$stats/signal").c_str(), 2, true, String(WiFi.RSSI()).c_str());
		}

		if(bDoPublishDefaults && (int) (millis()-ulPublishDefaultsTimestamp)>0)
		{
			bDoPublishDefaults=0;

			for(size_t a=0;a<vecNode.size();a++)
			{
				vecNode[a]->PublishDefaults();
			}
		}

	}
	else
	{

		ulHomieStatsTimestamp=millis()-1000000;

		if(!bConnecting)
		{

			unsigned long interval=5000;
			if(ulMqttReconnectCount>=20) interval=60000;
			else if(ulMqttReconnectCount>=15) interval=30000;
			else if(ulMqttReconnectCount>=10) interval=20000;
			else if(ulMqttReconnectCount>=5) interval=10000;

			if(!ulLastReconnect || (millis()-ulLastReconnect)>interval)
			{

#ifdef HOMIELIB_VERBOSE
				csprintf("Connecting...");
#endif
				bConnecting=true;
				mqtt.connect();
			}
		}

	}




}

void HomieDevice::onConnect(bool sessionPresent)
{
#ifdef HOMIELIB_VERBOSE
	csprintf("onConnect... %p\n",this);
#endif
	bConnecting=false;

	bDoInitialPublishing=true;

	if(sessionPresent)
	{
	}


}

void HomieDevice::onDisconnect(AsyncMqttClientDisconnectReason reason)
{
	if(reason==AsyncMqttClientDisconnectReason::TCP_DISCONNECTED)
	{
	}
	//csprintf("onDisconnect...");
	if(bConnecting)
	{
		ulLastReconnect=millis();
		ulMqttReconnectCount++;
		bConnecting=false;
		//csprintf("onDisconnect...   reason %i.. lr=%lu\n",reason,ulLastReconnect);
		csprintf("MQTT server connection failed\n");
	}
	else
	{
		csprintf("MQTT server connection lost\n");
	}
}

void HomieDevice::onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total)
{
	String strTopic=topic;
	_map_incoming::const_iterator citer=mapIncoming.find(strTopic);

	if(citer!=mapIncoming.end())
	{
		HomieProperty * pProp=citer->second;
		if(pProp)
		{

			pProp->OnMqttMessage(topic, payload, properties, len, index, total);

		}
	}


	//csprintf("RECEIVED %s %s\n",topic,payload);
}


HomieNode * HomieDevice::NewNode()
{
	HomieNode * ret=new HomieNode;
	vecNode.push_back(ret);
	ret->pParent=this;

	return ret;
}



void HomieDevice::DoInitialPublishing()
{
	if(!bDoInitialPublishing)
	{
		iInitialPublishing=0;
		ulInitialPublishing=0;
		return;
	}

	if(ulInitialPublishing!=0 && (millis()-ulInitialPublishing)<(unsigned long) iInitialPublishingThrottle_ms)
	{
		return;
	}

	if(!ulInitialPublishing)
	{
		csprintf("%s MQTT Initial Publishing...\n",strTopic.c_str());
		iPubCount_Props=0;
	}

	ulInitialPublishing=millis();


	if(!AllowInitialPublishing(this)) return;


#ifdef HOMIELIB_VERBOSE
	if(bDebug) csprintf("IPUB: %i        Node=%i  Prop=%i\n",iInitialPublishing, iInitialPublishing_Node, iInitialPublishing_Prop);
#endif


	const int ipub_qos=1;
	const int sub_qos=2;

	if(iInitialPublishing==0)
	{
		mqtt.publish(String(strTopic+"/$state").c_str(), ipub_qos, true, "init");
		mqtt.publish(String(strTopic+"/$homie").c_str(), ipub_qos, true, "3.0.1");
		mqtt.publish(String(strTopic+"/$name").c_str(), ipub_qos, true, strFriendlyName.c_str());
		iInitialPublishing=1;
		return;
	}

	if(iInitialPublishing==1)
	{
		mqtt.publish(String(strTopic+"/$localip").c_str(), ipub_qos, true, WiFi.localIP().toString().c_str());
		mqtt.publish(String(strTopic+"/$mac").c_str(), ipub_qos, true, WiFi.macAddress().c_str());
		mqtt.publish(String(strTopic+"/$extensions").c_str(), ipub_qos, true, "");
		iInitialPublishing=2;
		return;
	}

	if(iInitialPublishing==2)
	{

		mqtt.publish(String(strTopic+"/$stats").c_str(), ipub_qos, true, "uptime,signal");
		mqtt.publish(String(strTopic+"/$stats/interval").c_str(), ipub_qos, true, "60");

		String strNodes;
		for(size_t i=0;i<vecNode.size();i++)
		{
			strNodes+=vecNode[i]->strID;
			if(i<vecNode.size()-1) strNodes+=",";
		}

#ifdef HOMIELIB_VERBOSE
		if(bDebug) csprintf("NODES: %s\n",strNodes.c_str());
#endif

		mqtt.publish(String(strTopic+"/$nodes").c_str(), ipub_qos, true, strNodes.c_str());

		iInitialPublishing_Node=0;
		iInitialPublishing=3;
		return;
	}


	if(iInitialPublishing==3)
	{
		int i=iInitialPublishing_Node;
		iInitialPublishing_Node++;
		if(i<(int) vecNode.size())
		{
			HomieNode & node=*vecNode[i];
#ifdef HOMIELIB_VERBOSE
			if(bDebug) csprintf("NODE %i: %s\n",i,node.strFriendlyName.c_str());
#endif

			mqtt.publish(String(node.strTopic+"/$name").c_str(), ipub_qos, true, node.strFriendlyName.c_str());
			mqtt.publish(String(node.strTopic+"/$type").c_str(), ipub_qos, true, node.strType.c_str());

			String strProperties;
			for(size_t j=0;j<node.vecProperty.size();j++)
			{
				strProperties+=node.vecProperty[j]->strID;
				if(j<node.vecProperty.size()-1) strProperties+=",";
			}

#ifdef HOMIELIB_VERBOSE
			if(bDebug) csprintf("NODE %i: %s has properties %s\n",i,node.strFriendlyName.c_str(),strProperties.c_str());
#endif

			mqtt.publish(String(node.strTopic+"/$properties").c_str(), ipub_qos, true, strProperties.c_str());
			return;
		}

		if(i>=(int) vecNode.size())
		{
			iInitialPublishing=4;
			iInitialPublishing_Node=0;
			iInitialPublishing_Prop=0;
		}
	}

	if(iInitialPublishing==4)
	{
		int i=iInitialPublishing_Node;

		if(i<(int) vecNode.size())
		{
			HomieNode & node=*vecNode[i];
#ifdef HOMIELIB_VERBOSE
			if(bDebug) csprintf("NODE %i: %s\n",i,node.strFriendlyName.c_str());
#endif

			int j=iInitialPublishing_Prop;
			iInitialPublishing_Prop++;
			if(j<(int) node.vecProperty.size())
			{
				HomieProperty & prop=*node.vecProperty[j];

#ifdef HOMIELIB_VERBOSE
				if(bDebug) csprintf("NODE %i: %s property %s\n",i,node.strFriendlyName.c_str(),prop.strFriendlyName.c_str());
#endif

				mqtt.publish(String(prop.strTopic+"/$name").c_str(), ipub_qos, true, prop.strFriendlyName.c_str());
				mqtt.publish(String(prop.strTopic+"/$settable").c_str(), ipub_qos, true, prop.bSettable?"true":"false");
				mqtt.publish(String(prop.strTopic+"/$retained").c_str(), ipub_qos, true, prop.bRetained?"true":"false");
				mqtt.publish(String(prop.strTopic+"/$datatype").c_str(), ipub_qos, true, GetHomieDataTypeText(prop.datatype));
				if(prop.strUnit.length()) mqtt.publish(String(prop.strTopic+"/$unit").c_str(), ipub_qos, true, prop.strUnit.c_str());
				if(prop.strFormat.length()) mqtt.publish(String(prop.strTopic+"/$format").c_str(), ipub_qos, true, prop.strFormat.c_str());

				if(prop.bSettable)
				{
					mapIncoming[prop.strTopic]=&prop;
					mapIncoming[prop.strSetTopic]=&prop;
					if(prop.bRetained)
					{
#ifdef HOMIELIB_VERBOSE
						csprintf("SUBSCRIBING to %s\n",prop.strTopic.c_str());
#endif
						mqtt.subscribe(prop.strTopic.c_str(), sub_qos);
					}
#ifdef HOMIELIB_VERBOSE
					csprintf("SUBSCRIBING to %s\n",prop.strSetTopic.c_str());
#endif
					mqtt.subscribe(prop.strSetTopic.c_str(), sub_qos);
				}
				else
				{
					prop.Publish();
				}




				iPubCount_Props++;

				return;
			}

			if(iInitialPublishing_Prop>=(int) node.vecProperty.size())
			{
				iInitialPublishing_Prop=0;
				iInitialPublishing_Node++;
			}
		}

		if(iInitialPublishing_Node>=(int) vecNode.size())
		{
			iInitialPublishing_Node=0;
			iInitialPublishing_Prop=0;
			iInitialPublishing=5;
		}

	}

	if(iInitialPublishing==5)
	{
		mqtt.publish(String(strTopic+"/$state").c_str(), ipub_qos, true, "ready");
		bDoInitialPublishing=false;
		csprintf("Initial publishing complete. %i nodes, %i properties\n",vecNode.size(),iPubCount_Props);
		FinishInitialPublishing(this);

		ulPublishDefaultsTimestamp=millis()+5000;
		bDoPublishDefaults=true;

	}

}

void HomieDevice::PublishDirect(const String & topic, uint8_t qos, bool retain, const String & payload)
{
	mqtt.publish(topic.c_str(), qos, retain, payload.c_str(), payload.length());
}


String HomieDeviceName(const char * in)
{
	String ret;

	enum eLast
	{
		last_hyphen,
		last_upper,
		last_lower,
		last_number,
	};

	eLast last=last_hyphen;

	while(*in)
	{
		char input=*in++;

		if(input>='0' && input<='9')
		{
			ret+=input;
			last=last_number;
		}
		else if(input>='A' && input<='Z')
		{
			if(last==last_lower)
			{
				ret+='-';
			}

			ret+=(char) (input | 0x20);

			last=last_upper;
		}
		else if(input>='a' && input<='z')
		{
			ret+=input;
			last=last_lower;
		}
		else
		{
			if(last!=last_hyphen)
			{
				ret+="-";
			}

			last=last_hyphen;
		}

	}

	return ret;
}

bool HomieParseRGB(const char * in, uint32_t & rgb)
{
	int r, g, b;

	if (sscanf(in, "%d,%d,%d", &r, &g, &b) == 3)
	{
		rgb=(r<<16) + (g<<8) + (b<<0);
		return true;
	}
	return false;
}


void HSVtoRGB(float fHueIn, float fSatIn, float fBriteIn, float & fRedOut, float & fGreenOut, float & fBlueOut)
{
	float fHue = (float) fmod(fHueIn / 6.0f, 60);
	float fC = fBriteIn * fSatIn;
	float fL = fBriteIn - fC;
	float fX = (float) (fC * (1.0f - fabs(fmod(fHue * 0.1f, 2.0f) - 1.0f)));

	if(fHue >= 0.0f && fHue < 10.0f) { fRedOut = fC; fGreenOut = fX; fBlueOut = 0; }
	else if(fHue >= 10.0f && fHue < 20.0f) { fRedOut = fX; fGreenOut = fC; fBlueOut = 0; }
	else if(fHue >= 20.0f && fHue < 30.0f) { fRedOut = 0; fGreenOut = fC; fBlueOut = fX; }
	else if(fHue >= 30.0f && fHue < 40.0f) { fRedOut = 0; fGreenOut = fX; fBlueOut = fC; }
	else if(fHue >= 40.0f && fHue < 50.0f) { fRedOut = fX; fGreenOut = 0; fBlueOut = fC; }
	else if(fHue >= 50.0f && fHue < 60.0f) { fRedOut = fC; fGreenOut = 0; fBlueOut = fX; }
	else { fRedOut = 0; fGreenOut = 0; fBlueOut = 0; }

	fRedOut += fL;
	fGreenOut += fL;
	fBlueOut += fL;
}


bool HomieParseHSV(const char * in, uint32_t & rgb)
{
	int h, s, v;

	if (sscanf(in, "%d,%d,%d", &h, &s, &v) == 3)
	{
		float fR, fG, fB;

		HSVtoRGB(h, s*0.01f, v*0.01f,fR, fG, fB);

		int r=fR*256.0f;
		int g=fG*256.0f;
		int b=fB*256.0f;

		if(r>255) r=255;
		if(g>255) g=255;
		if(b>255) b=255;

		rgb=(r<<16) + (g<<8) + (b<<0);
		return true;
	}
	return false;
}

