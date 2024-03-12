//==========================================================================
// Name        : DatabasePlugin.cpp
// Author      : Battelle Memorial Institute
// Version     :
// Copyright   : Copyright (c) 2014 Battelle Memorial Institute. All rights reserved.
// Description : Example Plugin
//==========================================================================

#include "PluginClient.h"
#include "PluginDataMonitor.h"

#include <atomic>
#include <thread>
#include <DecodedBsmMessage.h>
#include <DatabaseMessage.h>
#include <tmx/j2735_messages/MapDataMessage.hpp>
#include <pqxx/pqxx>
#include <tmx/messages/auto_message.hpp>
#include <tmx/messages/routeable_message.hpp>

using namespace std;
using namespace tmx;
using namespace tmx::utils;
using namespace tmx::messages;

namespace DatabasePlugin
{

/**
 * This plugin is used to post PhantomTraffic data to our database.
 */
class DatabasePlugin: public PluginClient
{
public:
	DatabasePlugin(std::string);
	virtual ~DatabasePlugin();
	int Main();
protected:
	void UpdateConfigSettings();

	// Virtual method overrides.
	void OnConfigChanged(const char *key, const char *value);
	void OnStateChange(IvpPluginState state);

	void HandleDecodedBsmMessage(DecodedBsmMessage &msg, routeable_message &routeableMsg);
	void HandleDataChangeMessage(DataChangeMessage &msg, routeable_message &routeableMsg);
	void DummyInsertion();
	// void HandleDatabaseMessage(DatabaseMessage &msg, routeable_message &routeableMsg);
	void OnMessageReceived(IvpMessage *msg);
private:
	std::atomic<uint64_t> _frequency{0};
	DATA_MONITOR(_frequency);   // Declares the
};

/**
 * Construct a new DatabasePlugin with the given name.
 *
 * @param name The name to give the plugin for identification purposes.
 */
DatabasePlugin::DatabasePlugin(string name): PluginClient(name)
{
	// The log level can be changed from the default here.
	FILELog::ReportingLevel() = FILELog::FromString("DEBUG");

	// Add a message filter and handler for each message this plugin wants to receive.
	AddMessageFilter<DecodedBsmMessage>(this, &DatabasePlugin::HandleDecodedBsmMessage);

	// This is an internal message type that is used to track some plugin data that changes
	AddMessageFilter<DataChangeMessage>(this, &DatabasePlugin::HandleDataChangeMessage);

	// This is an internal message type that is used to send PhantomTrafficPlugin data to the database
	// AddMessageFilter<DatabaseMessage>(this, &DatabasePlugin::HandleDatabaseMessage);
	AddMessageFilter("Internal", "DatabaseMessage");

	// Subscribe to all messages specified by the filters above.
	SubscribeToMessages();
}

DatabasePlugin::~DatabasePlugin()
{
}

void DatabasePlugin::UpdateConfigSettings()
{
	// Configuration settings are retrieved from the API using the GetConfigValue template class.
	// This method does NOT execute in the main thread, so variables must be protected
	// (e.g. using std::atomic, std::mutex, etc.).

	int instance;
	GetConfigValue("Instance", instance);

	GetConfigValue("Frequency", __frequency_mon.get());
	__frequency_mon.check();
}


void DatabasePlugin::OnConfigChanged(const char *key, const char *value)
{
	PluginClient::OnConfigChanged(key, value);
	UpdateConfigSettings();
}

void DatabasePlugin::OnStateChange(IvpPluginState state)
{
	PluginClient::OnStateChange(state);

	if (state == IvpPluginState_registered)
	{
		UpdateConfigSettings();
		SetStatus("ReceivedMaps", 0);
	}
}

void DatabasePlugin::HandleDecodedBsmMessage(DecodedBsmMessage &msg, routeable_message &routeableMsg)
{
	DummyInsertion();	
}


// Example of handling
void DatabasePlugin::HandleDataChangeMessage(DataChangeMessage &msg, routeable_message &routeableMsg)
{
	PLOG(logINFO) << "Received a data change message: " << msg;

	PLOG(logINFO) << "Data field " << msg.get_untyped(msg.Name, "?") <<
			" has changed from " << msg.get_untyped(msg.OldValue, "?") <<
			" to " << msg.get_untyped(msg.NewValue, to_string(_frequency));
}

void DatabasePlugin::DummyInsertion() 
{
	try {
        // Connection parameters
        const std::string host = "127.0.1.1";
        const std::string port = "5488";
        const std::string dbname = "my_data_wh_db";
        const std::string user = "my_data_wh_user";
        const std::string password = "my_data_wh_pwd";

        // Construct the connection string
        std::string conn_string = "host=" + host + " port=" + port + " dbname=" + dbname +
                                  " user=" + user + " password=" + password;

        // Establish a connection
        pqxx::connection conn(conn_string);

        if (conn.is_open()) {
            // Create a transaction
            pqxx::work txn(conn);

            try {
                // Perform an INSERT operation
                txn.exec("INSERT INTO t_oil (region, country, year, production, consumption) VALUES ('TEST', 'TEST', 2021, 100, 100)");

                // Commit the transaction
                txn.commit();

                std::cout << "Data inserted successfully." << std::endl;
            } catch (const std::exception &e) {
                // Rollback the transaction in case of an error
                txn.abort();
                throw;
            }
        } else {
            std::cout << "Failed to open database" << std::endl;
        }
        

        // Close the connection when done
        conn.disconnect();
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}

void DatabasePlugin::OnMessageReceived(IvpMessage *msg) {

	PLOG(logDEBUG) << received IVP message in DB plugin << endl;

	routeable_message routeableMsg(msg);

	auto_message recv_msg;
	recv_msg.set_contents(routeableMsg.get_payload_str());

	// Parse the message and push to database
	// Get Fields from the message
	// Second parameter is the default value
	// -1 if unknown int or double

	// Timestamp
	uint64_t timestamp = recv_msg.get<uint64_t>("Timestamp", -1);

	// Number of vehicles in road segment
	int number_of_vehicles_in_road_segment = recv_msg.get<int>("NumberOfVehiclesInRoadSegment", -1);

	// Average speed of vehicles in road segment
	double average_speed_of_vehicles_in_road_segment = recv_msg.get<double>("AverageSpeedOfVehiclesInRoadSegment", -1.);

	// Speed limit of road segment
	double speed_limit_of_road_segment = recv_msg.get<double>("SpeedLimitOfRoadSegment", -1.);

	// Throughput of road segment
	double throughput_of_road_segment = recv_msg.get<double>("ThroughputOfRoadSegment", -1.);

	PLOG(logDEBUG) << "Got data: " << timestamp << " " << number_of_vehicles_in_road_segment << " " << average_speed_of_vehicles_in_road_segment << " " << speed_limit_of_road_segment << " " << throughput_of_road_segment << endl;

	// Push to database


}
/* 
 * Handle Incoming Database Message 
 * Parse data here and push to database using libcq++ function.
 */
// void DatabasePlugin::HandleDatabaseMessage(DatabaseMessage &msg, routeable_message &routeableMsg)
// {
// 	PLOG(logINFO) << "Received a database message: " << msg;

// 	// Parse the message and push to database
// 	// Get Fields from the message

// 	// Timestamp
// 	uint64_t timestamp = msg.get_Timestamp();

// 	// Number of vehicles in road segment
// 	int number_of_vehicles_in_road_segment = msg.get_NumberOfVehiclesInRoadSegment();

// 	// Average speed of vehicles in road segment
// 	double average_speed_of_vehicles_in_road_segment = msg.get_AverageSpeedOfVehiclesInRoadSegment();

// 	// Speed limit of road segment
// 	double speed_limit_of_road_segment = msg.get_SpeedLimitOfRoadSegment();

// 	// Throughput of road segment
// 	double throughput_of_road_segment = msg.get_ThroughputOfRoadSegment();

// 	// Push to database



// }


// Override of main method of the plugin that should not return until the plugin exits.
// This method does not need to be overridden if the plugin does not want to use the main thread.
int DatabasePlugin::Main()
{
	PLOG(logINFO) << "Starting plugin.";

	uint msCount = 0;
	DummyInsertion();
	while (_plugin->state != IvpPluginState_error)
	{
		PLOG(logDEBUG4) << "Sleeping 1 ms" << endl;

		this_thread::sleep_for(chrono::milliseconds(10));

		msCount += 10;

		// Example showing usage of _frequency configuraton parameter from main thread.
		// Access is thread safe since _frequency is declared using std::atomic.
		if (_plugin->state == IvpPluginState_registered && _frequency <= msCount)
		{
			PLOG(logINFO) << _frequency << " ms wait is complete.";
			msCount = 0;
		}
	}

	PLOG(logINFO) << "Plugin terminating gracefully.";
	return EXIT_SUCCESS;
}

} /* namespace DatabasePlugin */

int main(int argc, char *argv[])
{
	return run_plugin<DatabasePlugin::DatabasePlugin>("DatabasePlugin", argc, argv);
}
