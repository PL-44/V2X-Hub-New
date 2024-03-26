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

	private:
		std::atomic<uint64_t> _frequency{0};
		DATA_MONITOR(_frequency);					  // Declares the
		uint64_t vehicle_count;						  // vehicle count in the slowdown region
		vector<int32_t> vehicle_ids;				  // vehicle IDs in the slowdown region
		std::mutex vehicle_ids_mutex;				  // mutex for vehicle IDs
		tmx::utils::UdpClient *_signSimClient = NULL; // UDP client for sending speed limit to simulation
		std::map<int32_t, double> last_speeds;		  // map of vehicle IDs to their last speeds
		uint64_t number_of_vehicles_exited;			  // number of vehicles that have exited the slowdown region

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

		// This is an internal message type that is used to track some plugin data that changes
		AddMessageFilter<DataChangeMessage>(this, &PhantomTrafficPlugin::HandleDataChangeMessage);

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
		PLOG(logDEBUG) << "Received BSM Message: ";

		// Coordinates of the vehicle
		double vehicle_long = (double)(bsm->coreData.Long / 1000000.0 - 180);
		double vehicle_lat = (double)(bsm->coreData.lat / 1000000.0 - 180);

		double long_start = -123.185217; // Start of slowdown region
		double long_end = -123.178521;	 // End of slowdown region

		// Vehicle ID
		int32_t vehicle_id;
		memcpy(&vehicle_id, (unsigned char *)bsm->coreData.id.buf, 4);
		// GetInt32((unsigned char *)bsm->coreData.id.buf, &vehicle_id); // vehicle ID (

		// Lock the mutex
		std::lock_guard<std::mutex> lock(vehicle_ids_mutex);

		PLOG(logDEBUG) << "Got speed of vehicle " << vehicle_id << ": " << bsm->coreData.speed / 1000 << "m/s";

		last_speeds[vehicle_id] = (double) (bsm->coreData.speed / 1000); // Update the last speed of the vehicle

		// Check if the vehicle is in the slowdown region.
		if (vehicle_long >= long_start && vehicle_long <= long_end)
		{
			if (find(vehicle_ids.begin(), vehicle_ids.end(), vehicle_id) == vehicle_ids.end())
			{
				vehicle_ids.push_back(vehicle_id);
				vehicle_count += 1;
				PLOG(logDEBUG) << "Vehicle count in slowdown region: " << vehicle_count;
			}
		}
		else // Vehicle is not in the slowdown region
		{
			if (find(vehicle_ids.begin(), vehicle_ids.end(), vehicle_id) != vehicle_ids.end())
			{
				vehicle_ids.erase(remove(vehicle_ids.begin(), vehicle_ids.end(), vehicle_id), vehicle_ids.end());
				vehicle_count -= 1;
				number_of_vehicles_exited += 1;
				PLOG(logDEBUG) << "Vehicle count in slowdown region: " << vehicle_count;
			}
		}

		// The lock_guard automatically unlocks the mutex when it goes out of scope
	}

	// Example of handling
	void PhantomTrafficPlugin::HandleDataChangeMessage(DataChangeMessage &msg, routeable_message &routeableMsg)
	{
		PLOG(logINFO) << "Received a data change message: " << msg;

		PLOG(logINFO) << "Data field " << msg.get_untyped(msg.Name, "?") << " has changed from " << msg.get_untyped(msg.OldValue, "?") << " to " << msg.get_untyped(msg.NewValue, to_string(_frequency));
	}

	// Override of main method of the plugin that should not return until the plugin exits.
	// This method does not need to be overridden if the plugin does not want to use the main thread.
	int PhantomTrafficPlugin::Main()
	{
		PLOG(logINFO) << "Starting plugin.";

		const double original_speed = 25.0; // m/s
		uint16_t current_speed = original_speed;
		double average_speed = 0.0;

		uint16_t num_missing_heartbeat = 0;

		while (_plugin->state != IvpPluginState_error)
		{
			number_of_vehicles_exited = 0; // Reset the number of vehicles exited
			this_thread::sleep_for(chrono::milliseconds(MSG_INTERVAL * 1000));
			PLOG(logDEBUG) << "Phantom Alive!" << endl;

			// Only do work if the plugin is registered
			if (_plugin->state == IvpPluginState_registered && heartbeat)
			{
				heartbeat = false;
				sysreset = false;
				// Lock the mutex
				std::lock_guard<std::mutex> lock(vehicle_ids_mutex);

				// Calculate the average speed of vehicles in the slowdown region
				double last_average_speed = average_speed;
				average_speed = 0.0;
				int count = 0;
				PLOG(logDEBUG) << "Calculating average speed of vehicles in slowdown region.";
				for (int32_t vehicle_id : vehicle_ids)
				{
					average_speed += last_speeds[vehicle_id];
					count += 1;
				}
				PLOG(logDEBUG) << "Sum speed: " << average_speed << "m/s" << " Count: " << count << " vehicles in slowdown region";

				if (count > 0)
				{
					PLOG(logDEBUG) << "Calc average" << endl;
					average_speed /= count;
					PLOG(logDEBUG) << "Average speed: " << average_speed << "m/s";
				}
				else
				{
					average_speed = original_speed; // If no vehicles are in the slowdown region, set average speed to the original speed
					PLOG(logDEBUG) << "No vehicles in slowdown region. Average speed: " << average_speed << "m/s";
				}

				current_speed = average_speed; // Set the current speed to the average speed

				// Apply scaling factor
				current_speed = current_speed * NEW_SPEED_FACTOR;

				// Create Database Message to send to the Database Plugin
				double throughput = number_of_vehicles_exited / MSG_INTERVAL; // throughput = number of vehicles that has exited slowdown zone / message interval
				PLOG(logDEBUG) << "Throughput: " << throughput << " vehicles/second";
				uint64_t timestamp = Clock::GetMillisecondsSinceEpoch();

				//  Create auto message to send to the Database Plugin
				auto_message auto_db_message;
				auto_db_message.auto_attribute<DatabaseMessage>(timestamp, "Timestamp");
				auto_db_message.auto_attribute<DatabaseMessage>(vehicle_count, "NumberOfVehiclesInRoadSegment");
				auto_db_message.auto_attribute<DatabaseMessage>(average_speed, "AverageSpeedOfVehiclesInRoadSegment");
				auto_db_message.auto_attribute<DatabaseMessage>(current_speed, "SpeedLimitOfRoadSegment");
				auto_db_message.auto_attribute<DatabaseMessage>(throughput, "ThroughputOfRoadSegment");

				PLOG(logDEBUG) << "Database Auto Message created: " << auto_db_message << endl;

				// Refer: Plugin Programming Guide Page 13 for dynamic cast
				routeable_message rMsg;
				rMsg.set_type("Internal");
				rMsg.set_subtype("DatabaseMessage");
				rMsg.set_payload(auto_db_message); // json encoding
				this->BroadcastMessage(rMsg);

				PLOG(logDEBUG) << "Routeable DB Message sent" << endl;

				// Average speed always even (resolution of 2 m/s)
				average_speed -= (double) (((uint16_t) average_speed) % 2);

				PLOG(logDEBUG) << "Phantom Traffic Msg: " << average_speed << endl;

				// Only send if slow down detected with a non empty zone
				if (average_speed <= SLOW_DOWN_THRES && count > 0)
				{
					uint16_t new_speed = original_speed - (25./15.) * vehicle_count;
					if (new_speed < 5) new_speed = 5;
					std::string new_speed_str = std::to_string(new_speed);
					_signSimClient->Send(new_speed_str);
					PLOG(logDEBUG) << "New speed limit sent to simulation: " << new_speed_str;
				}

				// The lock_guard automatically unlocks the mutex when it goes out of scope
			}

			// Make sure to reset system speed and vehicle tracking once no heartbeat received
			else if (!heartbeat && !sysreset)
			{
				if (num_missing_heartbeat++ > 5)
				{
					reset_systemvars();
				}
			}
			else
			{
				num_missing_heartbeat = 0;
			}
		}

		PLOG(logINFO) << "Plugin terminating gracefully.";
		return EXIT_SUCCESS;
	}

} /* namespace PhantomTrafficPlugin */

int main(int argc, char *argv[])
{
	return run_plugin<PhantomTrafficPlugin::PhantomTrafficPlugin>("PhantomTrafficPlugin", argc, argv);
}
