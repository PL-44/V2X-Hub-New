/*
 * ImmediateForwardPlugin.cpp
 *
 *  Created on: Feb 26, 2016
 *      Author: ivp
 */

#include "ImmediateForwardPlugin.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/hex.hpp>

using namespace boost::algorithm; 
using namespace boost::property_tree;
using namespace std;
using namespace tmx;


// RSU has the ability to "sign every nth message" for encrypted messages

namespace ImmediateForward
{

const char* Key_SkippedNoDsrcMetadata = "Messages Skipped (No DSRC metadata)";
const char* Key_SkippedNoMessageRoute = "Messages Skipped (No route)";
const char* Key_SkippedSignError = "Message Skipped (Signature Error Response)";
const char* Key_SkippedInvalidUdpClient = "Messages Skipped (Invalid UDP Client)";


ImmediateForwardPlugin::ImmediateForwardPlugin(std::string name) : PluginClient(name),
	_configRead(false),
	_skippedNoDsrcMetadata(0),
	_skippedNoMessageRoute(0),
	_skippedInvalidUdpClient(0)
{
	AddMessageFilter("J2735", "*", IvpMsgFlags_RouteDSRC);
	AddMessageFilter("Battelle-DSRC", "*", IvpMsgFlags_RouteDSRC);
	SubscribeToMessages();

	_muteDsrc = false;
}

ImmediateForwardPlugin::~ImmediateForwardPlugin()
{
	lock_guard<mutex> lock(_mutexUdpClient);

	for (uint i = 0; i < _udpClientList.size(); i++)
	{
		for (uint j = 0; j < _udpClientList[i].size(); j++)
		{
			if (_udpClientList[i][i] != NULL)
				delete _udpClientList[i][j];
		}
	}

	for (uint i = 0; i < _SNMPClientList.size(); i++)
	{
		for (uint j = 0; j < _SNMPClientList[i].size(); j++)
		{
			if (_SNMPClientList[i][i] != NULL)
				delete _SNMPClientList[i][j];
		}
	}
	
}

// @SONAR_START@


void ImmediateForwardPlugin::OnConfigChanged(const char *key, const char *value)
{
	PluginClient::OnConfigChanged(key, value);

	UpdateConfigSettings();
}


void ImmediateForwardPlugin::OnMessageReceived(IvpMessage *msg)
{
	// Uncomment this line to call the base method, which prints the message received to cout.
	//PluginClient::OnMessageReceived(msg);
	PLOG(logDEBUG) << "Message Received " <<
				"Type: " << msg->type << ", Subtype: " << msg->subtype;
	if (!_configRead)
	{
		PLOG(logWARNING) << "Config not read yet.  Message Ignored: " <<
				"Type: " << msg->type << ", Subtype: " << msg->subtype;
		return;
	}

	if (msg->dsrcMetadata == NULL)
	{
		SetStatus<uint>(Key_SkippedNoDsrcMetadata, ++_skippedNoDsrcMetadata);
		PLOG(logWARNING) << "No DSRC metadata.  Message Ignored: " <<
				"Type: " << msg->type << ", Subtype: " << msg->subtype;
		return;
	}

	if(!_muteDsrc)
	{
		PLOG(logDEBUG) << "Sending message to radio...";
		SendMessageToRadio(msg);
	}
}


void ImmediateForwardPlugin::OnStateChange(IvpPluginState state)
{
	PluginClient::OnStateChange(state);

	if (state == IvpPluginState_registered)
	{
		UpdateConfigSettings();
	}
}

// Update config for SNMP clients
void ImmediateForwardPlugin::UpdateConfigSettings()
{
	PLOG(logINFO) << "Updating configuration settings.";

	// Update the configuration setting for all UDP clients.
	// This includes creation/update of _udpClientList and _messageConfigMap.
	{
		lock_guard<mutex> lock(_mutexUdpClient);
		_messageConfigMap.clear();

		_skippedNoDsrcMetadata = 0;
		_skippedNoMessageRoute = 0;
		_skippedInvalidUdpClient = 0;
		_skippedSignErrorResponse = 0;
		SetStatus<uint>(Key_SkippedNoDsrcMetadata, _skippedNoDsrcMetadata);
		SetStatus<uint>(Key_SkippedNoMessageRoute, _skippedNoMessageRoute);
		SetStatus<uint>(Key_SkippedInvalidUdpClient, _skippedInvalidUdpClient);
		SetStatus<uint>(Key_SkippedSignError, _skippedSignErrorResponse);
	}

	PLOG(logINFO) << "Updating client lists";
	if(snmpState == 1){ 
		for (uint i = 0; i < _SNMPClientList.size(); i++)
		{
			UpdateUdpClientFromConfigSettings(i);
		}
	}
	else{
		for (uint i = 0; i < _udpClientList.size(); i++)
		{
			UpdateUdpClientFromConfigSettings(i);
		}
	}

	// Get the signature setting.
	// The same mutex is used that protects the UDP clients.
	GetConfigValue<unsigned int>("signMessage", signState, &_mutexUdpClient);
	GetConfigValue<string>("HSMurl",baseurl, &_mutexUdpClient);
	string request="sign";
	url=baseurl+request;

	GetConfigValue("MuteDsrcRadio", _muteDsrc);
	SetStatus("MuteDsrc", _muteDsrc);
	_configRead = true;
}


// Retrieve all settings for a UDP client, then create a UDP client using those settings.
// Other settings related to the UDP client are also updated (i.e. msg id list, psid list).

// Update for SNMPClientList, keep RSU IPs sending to both clients as need
bool ImmediateForwardPlugin::UpdateUdpClientFromConfigSettings(uint clientIndex)
{
	// only check snmp if snmp is enabled
	if (_udpClientList.size() <= clientIndex)
	{
		PLOG(logWARNING) << "Invalid UDP client number. Only " << _udpClientList.size() << " clients available.";
		return false;
	}
	else if (_SNMPClientList.size() <= clientIndex) // updated
	{
		PLOG(logWARNING) << "Invalid SNMP client number. Only " << _SNMPClientList.size() << " clients available.";
		return false;
	}

	int clientNum = clientIndex + 1;
	string messagesSetting((boost::format("Messages_Destination_%d") % clientNum).str());
	string udpPortSetting((boost::format("Destination_%d") % clientNum).str());
	string SNMPPortSetting((boost::format("Destination_%d") % clientNum).str());


	PLOG(logDEBUG) << "SNMPPortSetting: " << SNMPPortSetting;
	try
	{
		string destinations;
		string messages;
		if(snmpState == 1){
			GetConfigValue(SNMPPortSetting, destinations);
		}
		else{
			GetConfigValue(udpPortSetting, destinations);
		}
		
		GetConfigValue(messagesSetting, messages);

		GetConfigValue<unsigned int>("EnableSNMP", snmpState);
		GetConfigValue<string>("SecurityLevel", _securityLevel);
		GetConfigValue<string>("SNMPUser", _snmpUser);
		GetConfigValue<string>("AuthPassPhrase", _authPassPhrase);

		// Take the lock while shared data is accessed.
		// A lock_guard will unlock when it goes out of scope (even if an exception occurs).
		lock_guard<mutex> lock(_mutexUdpClient);

		ParseJsonMessageConfig(messages, clientIndex);


		for (uint i = 0; i < _udpClientList[clientIndex].size(); i++)
		{
			if (_udpClientList[clientIndex][i] != NULL)
				delete _udpClientList[clientIndex][i];
		}
		for (uint i = 0; i < _SNMPClientList[clientIndex].size(); i++)
		{
			if (_SNMPClientList[clientIndex][i] != NULL)
				delete _SNMPClientList[clientIndex][i];
		}

		
		_udpClientList[clientIndex].clear();
		_SNMPClientList[clientIndex].clear();


		if (destinations.length() > 0)
		{
			vector<string> srvs;
			boost::split(srvs, destinations, boost::is_any_of(" \t,;"));

			for (uint i = 0; i < srvs.size(); i++)
			{
				vector<string> addr;
				boost::split(addr, srvs[i], boost::is_any_of(":"));
				if (addr.size() != 2)
					continue;
					
				if (snmpState == 1)
				{
					_rsuIp = addr[0];
					_snmpPort = stoul(addr[1]);
					PLOG(logINFO) << "Create SNMP Client to connect to RSU. RSU IP: " << _rsuIp << ",\tRSU Port: " << _snmpPort <<
							"\tSNMP User: " << _snmpUser << ",\tSecurity Level: " << _securityLevel << ",\tAuthentication Passphrase: " << _authPassPhrase << endl;
					// update SNMPClientList with the creation of a new SNMPClient with given params
					_SNMPClientList[clientIndex].push_back(new snmp_client(_rsuIp, _snmpPort, "public", _snmpUser, _securityLevel, _authPassPhrase, 3, 10000)); // updated
					PLOG(logDEBUG) << "Client added to list";
				}
				else
				{
					PLOG(logINFO) << "Creating UDP Client " << (clientIndex + 1) <<
							" - Radio IP: " << addr[0] << ", Port: " << addr[1];
					_udpClientList[clientIndex].push_back(new UdpClient(addr[0], ::atoi(addr[1].c_str())));
				}
			}
		}
	}
	catch(std::exception const & ex)
	{
		PLOG(logERROR) << "Error getting config settings: " << ex.what();
		return false;
	}

	return true;
}

bool ImmediateForwardPlugin::ParseJsonMessageConfig(const std::string& json, uint clientIndex)
{
	if (json.length() == 0)
		return true;

	try
	{
		//delete all MessageConfig for this client
		for (auto it = _messageConfigMap.begin(); it != _messageConfigMap.end(); )
		{
			 if (it->ClientIndex == clientIndex)
				 it = _messageConfigMap.erase(it);
			 else
				 ++it;

		}

		// Example JSON parsed:
		// { "Messages": [ { "TmxType": "MAP-P", "SendType": "MAP", "PSID": "0x8002", "Channel": "172" }, { "TmxType": "SPAT-P", "SendType": "SPAT", "PSID": "0x8002" } ] }
		// The strings below (with extra quotes escaped) can be used for testing.
		//string json2 = "{ "Messages": [ ] }";
		//string json2 = "{ "Messages": [ { "TmxType": "MAP-P", "SendType": "MAP", "PSID": "0x8002" }, { "TmxType": "SPAT-P", "SendType": "SPAT", "PSID": "0x8002" } ] }";

		// Read the JSON into a boost property tree.
		ptree pt;
		istringstream is(json);
		read_json(is, pt);

		// Iterate over the Messages section of the property tree.
		// Note that Messages is at the root of the property tree, otherwise the entire
		// path to the child would have to be specified to get_child.
		BOOST_FOREACH(ptree::value_type &child, pt.get_child("Messages"))
		{
			// Array elements have no names.
			assert(child.first.empty());

			MessageConfig config;
			config.ClientIndex = clientIndex;
			config.TmxType = child.second.get<string>("TmxType");
			config.SendType = child.second.get<string>("SendType");
			config.Psid = child.second.get<string>("PSID");
			try
			{
				config.Channel = child.second.get<string>("Channel");
			}
			catch(std::exception const & exChannel)
			{
				config.Channel.clear();
			}

			PLOG(logINFO) << "Message Config - Client: " << (config.ClientIndex + 1) <<
					", TmxType: " << config.TmxType << ", SendType: " << config.SendType << ", PSID: " << config.Psid <<
					", Channel: " << config.Channel;

			// Add the message configuration to the map.
			_messageConfigMap.push_back(config);
		}
	}
	catch(std::exception const & ex)
	{
		PLOG(logERROR) << "Error parsing Messages: " << ex.what();
		return false;
	}

	return true;
}

void ImmediateForwardPlugin::SendMessageToRadio(IvpMessage *msg)
{
	bool foundMessageType = false;
	static FrequencyThrottle<std::string> _statusThrottle(chrono::milliseconds(2000));

	lock_guard<mutex> lock(_mutexUdpClient);

	int msgCount = 0;

	std::map<std::string, int>::iterator itMsgCount = _messageCountMap.find(msg->subtype);

	if(itMsgCount != _messageCountMap.end())
	{
		msgCount = (int)itMsgCount->second;
		msgCount ++;
	}

	_messageCountMap[msg->subtype] = msgCount;


	if (_statusThrottle.Monitor(msg->subtype)) {
		SetStatus<int>(msg->subtype, msgCount);
	}

	// Convert the payload to upper case.
	for (int i = 0; i < (int)(strlen(msg->payload->valuestring)); i++)
		msg->payload->valuestring[i] = toupper(msg->payload->valuestring[i]);

	PLOG(logWARNING)<<_messageConfigMap.size();
	//loop through all MessageConfig and send to each with the proper TmxType
	for (int configIndex = 0;configIndex < _messageConfigMap.size();configIndex++)
	{	
		PLOG(logWARNING)<<_messageConfigMap[configIndex].TmxType;
		if (_messageConfigMap[configIndex].TmxType == msg->subtype)
		{
			foundMessageType = true;
			string payloadbyte="";


			// Format the message using the protocol defined in the
			// USDOT Roadside Unit Specifications Document v 4.0 Appendix C.

			stringstream os;

			/// if signing is Enabled, request signing with HSM 
			

			if (signState == 1)
			{
				std::string mType = _messageConfigMap[configIndex].SendType; 

				std::for_each(mType.begin(), mType.end(), [](char & c){
					c = ::tolower(c);
				});
				/* convert to hex array */

				string msgString=msg->payload->valuestring;
				string base64str=""; 

				hex2base64(msgString,base64str);  

				std::string req = "\'{\"type\":\""+mType+"\",\"message\":\""+base64str+"\"}\'";


				string cmd1="curl -X POST "+url+" -H \'Content-Type: application/json\' -d "+req; 
				const char *cmd=cmd1.c_str();  
				char buffer[2048];
				std::string result="";
				FILE* pipe= popen(cmd,"r"); 

				if (pipe == NULL ) 
					throw std::runtime_error("popen() failed!");
				try{
					while (fgets(buffer, sizeof(buffer),pipe) != NULL)
					{
						result+=buffer; 
					}
				} catch (std::exception const & ex) {
					
					pclose(pipe);
					SetStatus<uint>(Key_SkippedSignError, ++_skippedSignErrorResponse);
					PLOG(logERROR) << "Error parsing Messages: " << ex.what();
					return; 
				}
				PLOG(logDEBUG) << "SCMS Contain response = " << result << std::endl;
				cJSON *root   = cJSON_Parse(result.c_str());
				// Check if status is 200 (successful)
				cJSON *status = cJSON_GetObjectItem(root, "code");
				if ( status ) {
					// IF status code exists this means the SCMS container returned an error response on attempting to sign
					// Set status will increment the count of message skipped due to signature error responses by one each
					// time this occurs. This count will be visible under the "State" tab of this plugin.
					cJSON *message = cJSON_GetObjectItem(root, "message");
					SetStatus<uint>(Key_SkippedSignError, ++_skippedSignErrorResponse);
					PLOG(logERROR) << "Error response from SCMS container HTTP code " << status->valueint << "!\n" << message->valuestring << std::endl;
					return;
				}
				cJSON *sd = cJSON_GetObjectItem(root, "signedMessage");
				string signedMsg = sd->valuestring;
				base642hex(signedMsg,payloadbyte); // this allows sending hex of the signed message rather than base64

			}
			else
			{
				payloadbyte=msg->payload->valuestring; 
			}
			// @SONAR_START@


			// Send the message using the configured SNMP clients
			if (snmpState == 1)
			{
				auto request = tmx::utils::request_type::SET;
				auto type_INT = tmx::utils::snmp_response_obj::response_type::INTEGER;
				auto type_STR = tmx::utils::snmp_response_obj::response_type::STRING;

				auto snmpRsuMode = snmp_client(_rsuIp, _snmpPort, "public", _snmpUser, _securityLevel, _authPassPhrase, 3, 10000);
				tmx::utils::snmp_response_obj modeMessage;
				modeMessage.type = type_INT;
				modeMessage.val_int = 2;


				for(int i = 0; i < _SNMPClientList[_messageConfigMap[configIndex].ClientIndex].size(); i++){
					PLOG(logINFO) << "Current client in list: " << i;
					

					// Check to see if the client exists, and if so, set the message to it. Does the index of the forward table need incremented?
					if(_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i] != NULL){

						// Initial set to enable standby mode on a given RSU
						// snmpRsuMode->process_snmp_request("iso.0.15628.4.1.99.0", request, modeMessage);

						// Some of this will change with NTCIP
						PLOG(logDEBUG2) << _logPrefix << "Sending - TmxType: " << _messageConfigMap[configIndex].TmxType << ", SendType: " << _messageConfigMap[configIndex].SendType
							<< ", PSID: " << _messageConfigMap[configIndex].Psid << ", Client: " << _messageConfigMap[configIndex].ClientIndex
							<< ", Channel: " << (_messageConfigMap[configIndex].Channel.empty() ? ::to_string( msg->dsrcMetadata->channel) : _messageConfigMap[configIndex].Channel)
							<< ", Port: " << _SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->get_port();

						// Get the max number of possible objects in the forward table
						tmx::utils::snmp_response_obj maxIFMs;
						maxIFMs.type = type_INT;
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.1", tmx::utils::request_type::GET, maxIFMs);

						// The following commands are reflective of NTCIP spec, NOT the current spec.
						tmx::utils::snmp_response_obj msgEnableObj;
						msgEnableObj.type = type_INT;
						msgEnableObj.val_int = 1; // should always be set to enable
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.4", request, msgEnableObj);

						tmx::utils::snmp_response_obj msgIndexObj;
						msgIndexObj.type = type_INT;
						msgIndexObj.val_int = 0; // needs updating
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.1", request, msgIndexObj);

						tmx::utils::snmp_response_obj psidObj;
						psidObj.type = type_STR;
						std::vector<char> psidVector(_messageConfigMap[configIndex].Psid.begin(), _messageConfigMap[configIndex].Psid.end());
						psidObj.val_string = psidVector;
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.2", request, psidObj);

						tmx::utils::snmp_response_obj channelObj;
						channelObj.type = type_INT;
						channelObj.val_int = stoi(_messageConfigMap[configIndex].Channel);
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.3", request, channelObj);

						tmx::utils::snmp_response_obj statusObj;
						statusObj.type = type_INT;
						statusObj.val_int = 1;
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.5", request, statusObj);

						// //Payload - removed per updated NTCIP 1218 spec as of 11/06/23
						// tmx::utils::snmp_response_obj payloadObj;
						// payloadObj.type = type_STR;
						// std::vector<char> payloadVector(payloadbyte.begin(), payloadbyte.end());
						// payloadObj.val_string = payloadVector;
						// _SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.8", request, payloadObj);

						// TBD if needed in the final IFT update, listed for convienence
						
						// TODO: add parameter for message priority
						tmx::utils::snmp_response_obj priorityObj;
						priorityObj.type = type_INT;
						priorityObj.val_int = 7;
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.6", request, priorityObj);
						
						// TODO: add logic for handling the options
						std::vector<char> bits0_3 = {0, 1, 0, 0}; // to be replaced with switch case
						tmx::utils::snmp_response_obj optionsObj;
						optionsObj.type = type_STR; // will need to be 4 bits, add a swtich case to support
						optionsObj.val_string = bits0_3;
						_SNMPClientList[_messageConfigMap[configIndex].ClientIndex][i]->process_snmp_request("1.3.6.1.4.1.1206.4.2.18.4.2.1.7", request, optionsObj);
						

						// Current DRSC Spec
						// rsuIFMIndex		iso.0.15628.4.1.5.1.1 			RsuTableIndex
						// rsuIFMPsid		iso.0.15628.4.1.5.1.2			RsuPsidTC - string
						// rsuIFMDsrcMsgId	iso.0.15628.4.1.5.1.3			Int32
						// rsuIFMTxMode		iso.0.15628.4.1.5.1.4			INT
						// rsuIFMTxChannel	iso.0.15628.4.1.5.1.5			Int32
						// rsuIFMEnable		iso.0.15628.4.1.5.1.6			INT
						// rsuIFMStatus		iso.0.15628.4.1.5.1.7			RowStatus

						// NTCIP Spec
						// Msg index -	1.3.6.1.4.1.1206.4.2.18.4.2.1.1		RsuTableIndex - int32
						// PSID -		1.3.6.1.4.1.1206.4.2.18.4.2.1.2		RsuPsidTC (J2735 hex) - octet string
						// Channel - 	1.3.6.1.4.1.1206.4.2.18.4.2.1.3		int32
						// Msg enable -	1.3.6.1.4.1.1206.4.2.18.4.2.1.4		INTEGER
						// Status		1.3.6.1.4.1.1206.4.2.18.4.2.1.5		RowStatus
						// Priority		1.3.6.1.4.1.1206.4.2.18.4.2.1.6		int32
						// Options 		1.3.6.1.4.1.1206.4.2.18.4.2.1.7		(used to enabled signed or encrypted messages + protocol, 4 bit binary)
						// Payload - 	1.3.6.1.4.1.1206.4.2.18.4.2.1.8		octet string

					}
					else{
						PLOG(logWARNING) << "SNMP client invalid";
					}

				}
			}

			// Send the message using the configured UDP client.
			else
			{
				os << "Version=0.7" << "\n";
				os << "Type=" << _messageConfigMap[configIndex].SendType << "\n" << "PSID=" << _messageConfigMap[configIndex].Psid << "\n";
				if (_messageConfigMap[configIndex].Channel.empty())
					os << "Priority=7" << "\n" << "TxMode=CONT" << "\n" << "TxChannel=" << msg->dsrcMetadata->channel << "\n";
				else
					os << "Priority=7" << "\n" << "TxMode=CONT" << "\n" << "TxChannel=" << _messageConfigMap[configIndex].Channel << "\n";
				os << "TxInterval=0" << "\n" << "DeliveryStart=\n" << "DeliveryStop=\n";
				os << "Signature="<< (signState == 1 ? "True" : "False") << "\n" << "Encryption=False\n";
				os << "Payload=" << payloadbyte << "\n";

				string message = os.str(); // finalized message to send to clients

				for (uint i = 0; i < _udpClientList[_messageConfigMap[configIndex].ClientIndex].size(); i++)
				{
					//cout << message << endl;

					if (_udpClientList[_messageConfigMap[configIndex].ClientIndex][i] != NULL)
					{
						PLOG(logDEBUG2) << _logPrefix << "Sending - TmxType: " << _messageConfigMap[configIndex].TmxType << ", SendType: " << _messageConfigMap[configIndex].SendType
							<< ", PSID: " << _messageConfigMap[configIndex].Psid << ", Client: " << _messageConfigMap[configIndex].ClientIndex
							<< ", Channel: " << (_messageConfigMap[configIndex].Channel.empty() ? ::to_string( msg->dsrcMetadata->channel) : _messageConfigMap[configIndex].Channel)
							<< ", Port: " << _udpClientList[_messageConfigMap[configIndex].ClientIndex][i]->GetPort();

						_udpClientList[_messageConfigMap[configIndex].ClientIndex][i]->Send(message); // sending message to client
					}
					else
					{
						SetStatus<uint>(Key_SkippedInvalidUdpClient, ++_skippedInvalidUdpClient);
						PLOG(logWARNING) << "UDP Client Invalid. Cannot send message. TmxType: " << _messageConfigMap[configIndex].TmxType;
					}
				}
			}
		}
	}
	if (!foundMessageType)
	{
		SetStatus<uint>(Key_SkippedNoMessageRoute, ++_skippedNoMessageRoute);
		PLOG(logWARNING)<<" WARNING TMX Subtype not found in configuration. Message Ignored: " <<
				"Type: " << msg->type << ", Subtype: " << msg->subtype;
		return;
	}

}

} /* namespace ImmediateForward */