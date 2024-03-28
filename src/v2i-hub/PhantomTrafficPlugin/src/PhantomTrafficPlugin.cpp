//==========================================================================
// Name        : PhantomTrafficPlugin.cpp
// Author      : PL-44 Capstone Team, UBC Vancouver
// Version     : 1.0
// Copyright   : Copyright (c) 2014 Battelle Memorial Institute. All rights reserved.
// Description : Phantom Traffic Plugin
//==========================================================================

#include "PluginClient.h"
#include "PluginDataMonitor.h"

#include <atomic>
#include <thread>
#include <DecodedBsmMessage.h>
#include <DatabaseMessage.h>
#include <UdpClient.h>
#include "Clock.h"
#include <tmx/j2735_messages/BasicSafetyMessage.hpp>
#include <BasicSafetyMessage.h>
#include <tmx/messages/auto_message.hpp>
#include <tmx/messages/routeable_message.hpp>
#include <map>

using namespace std;
using namespace tmx;
using namespace tmx::utils;
using namespace tmx::messages;

#define MSG_INTERVAL 1 // 1 seconds
#define SLOW_DOWN_THRES 10
#define NEW_SPEED_FACTOR 1
#define MAX_MISSING_HEARTBEAT 5
#define STALE_THRESHOLD 3 * 1000 // 3 seconds
#define MAX_SLOWDOWN 20			 // m/s
#define MAX_VEHICLES_IN_SLOWDOWN 15
#define SLOWDOWN_FACTOR (double)((double)MAX_SLOWDOWN / (double)MAX_VEHICLES_IN_SLOWDOWN)

namespace PhantomTrafficPlugin
{

	/**
	 * This plugin observes vehicle counts in a slowdown region and slows down vehicles prior to it.
	 */
	class PhantomTrafficPlugin : public PluginClient
	{
	public:
		PhantomTrafficPlugin(std::string);
		virtual ~PhantomTrafficPlugin();
		int Main();

	protected:
		void UpdateConfigSettings();

		// Virtual method overrides.
		void OnConfigChanged(const char *key, const char *value);
		void OnStateChange(IvpPluginState state);

		void HandleBasicSafetyMessage(BsmMessage &msg, routeable_message &routeableMsg);
		void HandleDataChangeMessage(DataChangeMessage &msg, routeable_message &routeableMsg);
		void GetInt32(unsigned char *buf, int32_t *value)
		{
			*value = (int32_t)((buf[0] << 24) + (buf[1] << 16) + (buf[2] << 8) + buf[3]);
		}

		void reset_systemvars(void);
		void InitializePlugin(void);
		void CleanupStaleVehicles(void);
		void CalculateAverageSpeed(void);
		void AdjustSpeedLimit(void);
		void ProcessTrafficData(void);
		void SendDatabaseMessage(void);
		void HandleHeartbeat(void);

	private:
		std::atomic<uint64_t> _frequency{0};
		DATA_MONITOR(_frequency);					  // Declares the
		uint64_t vehicle_count;						  // vehicle count in the slowdown region
		std::mutex vehicle_ids_mutex;				  // mutex for vehicle IDs
		tmx::utils::UdpClient *_signSimClient = NULL; // UDP client for sending speed limit to simulation
		std::map<uint32_t, uint64_t> vehicle_ids;	  // map of vehicle IDs to the time of their message
		std::map<uint32_t, double> last_speeds;		  // map of vehicle IDs to their last speeds
		uint64_t number_of_vehicles_exited;			  // number of vehicles that have exited the slowdown region
		const double original_speed = 25.0;			  // m/s
		uint16_t current_speed;
		double average_speed;
		uint16_t num_missing_heartbeat;
		double previous_sent_speed = 0;
		double throughput = 0;
		bool heartbeat = false; // true when it receives message from phantom traffic
		bool sysreset = false;	// is system in reset state
	};

	/**
	 * Construct a new PhantomTrafficPlugin with the given name.
	 *
	 * @param name The name to give the plugin for identification purposes.
	 */
	PhantomTrafficPlugin::PhantomTrafficPlugin(string name) : PluginClient(name)
	{
		// The log level can be changed from the default here.
		FILELog::ReportingLevel() = FILELog::FromString("DEBUG");

		// Add a message filter and handler for each message this plugin wants to receive.
		AddMessageFilter<BsmMessage>(this, &PhantomTrafficPlugin::HandleBasicSafetyMessage);

		// Subscribe to all messages specified by the filters above.
		SubscribeToMessages();

		vehicle_count = 0; // Set initial vehicle count to 0 upon creation of plugin.

		number_of_vehicles_exited = 0; // Set initial number of vehicles exited to 0 upon creation of plugin.

		// Create UDP client for sending speed limit to simulation
		const std::string &address = "127.0.0.1"; // localhost
		int port = 4500;						  // port 4500
		_signSimClient = new tmx::utils::UdpClient(address, port);
	}

	PhantomTrafficPlugin::~PhantomTrafficPlugin()
	{
	}

	void PhantomTrafficPlugin::UpdateConfigSettings()
	{
		// Configuration settings are retrieved from the API using the GetConfigValue template class.
		// This method does NOT execute in the main thread, so variables must be protected
		// (e.g. using std::atomic, std::mutex, etc.).

		int instance;
		GetConfigValue("Instance", instance);

		GetConfigValue("Frequency", __frequency_mon.get());
		__frequency_mon.check();
	}

	void PhantomTrafficPlugin::OnConfigChanged(const char *key, const char *value)
	{
		PluginClient::OnConfigChanged(key, value);
		UpdateConfigSettings();
	}

	void PhantomTrafficPlugin::OnStateChange(IvpPluginState state)
	{
		PluginClient::OnStateChange(state);

		if (state == IvpPluginState_registered)
		{
			UpdateConfigSettings();
		}
	}

	void PhantomTrafficPlugin::reset_systemvars(void)
	{
		std::lock_guard<std::mutex> lock(vehicle_ids_mutex);

		PLOG(logDEBUG) << "SYS RESET" << endl;

		vehicle_count = 0;
		vehicle_ids.clear();
		last_speeds.clear();
		number_of_vehicles_exited = 0;

		heartbeat = false;
		sysreset = true;

		// std::lock_guard<std::mutex> unlock(vehicle_ids_mutex);
	}

	void PhantomTrafficPlugin::HandleBasicSafetyMessage(BsmMessage &msg, routeable_message &routeableMsg)
	{
		heartbeat = true;
		// Decode the BSM message
		std::shared_ptr<BasicSafetyMessage> bsm_shared = msg.get_j2735_data();
		BasicSafetyMessage *bsm = bsm_shared.get();

		// Coordinates of the vehicle
		double vehicle_long = (double)(bsm->coreData.Long / 1000000.0 - 180);
		double vehicle_lat = (double)(bsm->coreData.lat / 1000000.0 - 180);

		double long_start = -123.185217; // Start of slowdown region
		double long_end = -123.178521;	 // End of slowdown region

		// Vehicle ID
		uint32_t vehicle_id;
		memcpy(&vehicle_id, (unsigned char *)bsm->coreData.id.buf, 4);
		// GetInt32((unsigned char *)bsm->coreData.id.buf, &vehicle_id); // vehicle ID (

		// Lock the mutex
		std::lock_guard<std::mutex> lock(vehicle_ids_mutex);

		last_speeds[vehicle_id] = (double)(bsm->coreData.speed / 1000); // Update the last speed of the vehicle

		// Check if the vehicle is in the slowdown region.
		if (vehicle_long >= long_start && vehicle_long <= long_end)
		{
			if (vehicle_ids.count(vehicle_id) == 0) // Vehicle is in the slowdown region, but not tracked yet
			{
				PLOG(logDEBUG) << "Vehicle ID: " << vehicle_id << " is in the slowdown region." << endl;
				vehicle_ids[vehicle_id] = Clock::GetMillisecondsSinceEpoch(); // Add the vehicle to the map of vehicle IDs
				vehicle_count += 1;											  // Increment the vehicle count
			}
			else // Vehicle is already in the slowdown region
			{
				vehicle_ids[vehicle_id] = Clock::GetMillisecondsSinceEpoch(); // Update the time of the vehicle message
			}
		}
		// else do nothing. Vehicle is not in the slowdown region.

		// The lock_guard automatically unlocks the mutex when it goes out of scope
	}

	void PhantomTrafficPlugin::InitializePlugin()
	{
		PLOG(logINFO) << "Starting plugin.";

		current_speed = original_speed;
		average_speed = 0.0;

		num_missing_heartbeat = 0;
		previous_sent_speed = 0;
	}

	void PhantomTrafficPlugin::CleanupStaleVehicles()
	{
		uint64_t current_time = Clock::GetMillisecondsSinceEpoch();
		for (auto it = vehicle_ids.begin(); it != vehicle_ids.end();)
		{
			if (current_time - it->second > STALE_THRESHOLD) // 3 seconds threshold
			{
				it = vehicle_ids.erase(it);
				vehicle_count -= 1;
			}
			else
			{
				++it; // Only increment if not erased because erase already increments the iterator
			}
		}
	}

	void PhantomTrafficPlugin::CalculateAverageSpeed()
	{
		average_speed = 0.0;
		int count = 0;
		for (int32_t vehicle_id : vehicle_ids) 
		{
			average_speed += last_speeds[vehicle_id];
			count++;
		}
		average_speed = (count > 0) ? (double)(average_speed / (double)count) : original_speed;
		average_speed -= (double)(((uint16_t)average_speed) % 2);
		current_speed = average_speed * NEW_SPEED_FACTOR;
	}

	void PhantomTrafficPlugin::AdjustSpeedLimit()
	{
		// Only send if slow down detected with a non empty zone
		if (average_speed <= SLOW_DOWN_THRES && vehicle_ids.size() > 0)
		{
			double reduction = (SLOWDOWN_FACTOR * vehicle_count);
			uint16_t new_speed = original_speed;
			if (reduction >= MAX_SLOWDOWN)
				new_speed = 5;
			else
				new_speed -= reduction;
			std::string new_speed_str = std::to_string(new_speed);
			_signSimClient->Send(new_speed_str);
			previous_sent_speed = new_speed;
			PLOG(logDEBUG) << "New speed limit sent to simulation: " << new_speed_str << " Count: " << vehicle_count << " Average Speed: " << average_speed << endl;
		}
		else
		{
			// 	// if (previous_sent_speed != original_speed) { // enable if you want to limit messages sent to simulation
			PLOG(logDEBUG) << "Original speed limit sent to simulation: " << original_speed << "m/s" << endl;
			std::string original = std::to_string(original_speed);
			previous_sent_speed = original_speed;
			_signSimClient->Send(original);
			// 	// }
		}
	}

	void PhantomTrafficPlugin::ProcessTrafficData()
	{
		std::lock_guard<std::mutex> lock(vehicle_ids_mutex);
		CleanupStaleVehicles();
		CalculateAverageSpeed();
	}

	void PhantomTrafficPlugin::SendDatabaseMessage()
	{
		// Create Database Message to send to the Database Plugin
		throughput = number_of_vehicles_exited / MSG_INTERVAL; // throughput = number of vehicles that has exited slowdown zone / message interval
		uint64_t timestamp = Clock::GetMillisecondsSinceEpoch();

		//  Create auto message to send to the Database Plugin
		auto_message auto_db_message;
		auto_db_message.auto_attribute<DatabaseMessage>(timestamp, "Timestamp");
		auto_db_message.auto_attribute<DatabaseMessage>(vehicle_count, "NumberOfVehiclesInRoadSegment");
		auto_db_message.auto_attribute<DatabaseMessage>(average_speed, "AverageSpeedOfVehiclesInRoadSegment");
		auto_db_message.auto_attribute<DatabaseMessage>(current_speed, "SpeedLimitOfRoadSegment");
		auto_db_message.auto_attribute<DatabaseMessage>(throughput, "ThroughputOfRoadSegment");

		// Create routeable message to send to the Database Plugin
		routeable_message rMsg;
		rMsg.set_type("Internal");
		rMsg.set_subtype("DatabaseMessage");
		rMsg.set_payload(auto_db_message); // json encoding
		this->BroadcastMessage(rMsg);
	}

	void PhantomTrafficPlugin::HandleHeartbeat()
	{
		if (_plugin->state == IvpPluginState_registered)
		{
			PLOG(logDEBUG) << "Phantom Traffic Plugin Alive!" << endl;
			ProcessTrafficData();
			SendDatabaseMessage();
			AdjustSpeedLimit();
		}
		// Make sure to reset system speed and vehicle tracking once no heartbeat received
		else if (!heartbeat && !sysreset)
		{
			if (num_missing_heartbeat++ > MAX_MISSING_HEARTBEAT)
			{
				reset_systemvars();
			}
		}
		else
		{
			heartbeat = false;
			sysreset = false;

			num_missing_heartbeat = 0;
		}
	}

	// Override of main method of the plugin that should not return until the plugin exits.
	// This method does not need to be overridden if the plugin does not want to use the main thread.
	int PhantomTrafficPlugin::Main()
	{
		InitializePlugin();

		while (_plugin->state != IvpPluginState_error)
		{
			this_thread::sleep_for(chrono::milliseconds(MSG_INTERVAL * 1000));
			HandleHeartbeat();
		}

		PLOG(logINFO) << "Plugin terminating gracefully.";
		return EXIT_SUCCESS;
	}

} /* namespace PhantomTrafficPlugin */

int main(int argc, char *argv[])
{
	return run_plugin<PhantomTrafficPlugin::PhantomTrafficPlugin>("PhantomTrafficPlugin", argc, argv);
}
