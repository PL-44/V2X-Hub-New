#include <gtest/gtest.h>
#include "SNMPClient.h"

using namespace tmx::utils;

namespace unit_test
{
    class SNMPClientTest : public ::testing::Test
    {
    };

    TEST_F(SNMPClientTest, constructor)
    {
        const std::string rsu_ip = "127.0.0.1";
        uint16_t snmp_port = 161;
        std::string snmp_user = "dummy";
        std::string securityLevel = "authPriv";
        std::string authPassPhrase = "dummy"; // Error: passphrase chosen is below the length requirements of the USM (min=8).
        std::string communityTest = "public"; //remove after test
        // ASSERT_ANY_THROW(SNMPClient(rsu_ip, snmp_port, snmp_user, securityLevel, authPassPhrase));
        ASSERT_ANY_THROW(snmp_client(rsu_ip, snmp_port, communityTest, snmp_user, securityLevel, authPassPhrase, 3, 1000));
        authPassPhrase = "dummydummy";
        // ASSERT_NO_THROW(SNMPClient(rsu_ip, snmp_port, snmp_user, securityLevel, authPassPhrase));
        ASSERT_NO_THROW(snmp_client(rsu_ip, snmp_port, communityTest, snmp_user, securityLevel, authPassPhrase, 3, 1000));
    }
    
    TEST_F(SNMPClientTest, SNMPGet)
    {
        const std::string rsu_ip = "127.0.0.1";
        uint16_t snmp_port = 161;
        std::string snmp_user = "dummy";
        std::string securityLevel = "authPriv";
        std::string authPassPhrase = "dummydummy";
        std::string communityTest = "public"; //remove after test
        // auto snmpClient = SNMPClient(rsu_ip, snmp_port, snmp_user, securityLevel, authPassPhrase);
        auto snmpClient = snmp_client(rsu_ip, snmp_port, communityTest, snmp_user, securityLevel, authPassPhrase, 3, 1000);
        // ASSERT_THROW(snmpClient.SNMPGet("1.0.15628.4.1.8.5.0"), SNMPClientException);
        ASSERT_THROW(snmpClient.SNMPGet("1.0.15628.4.1.8.5.0"), snmp_client_exception);
    }
}