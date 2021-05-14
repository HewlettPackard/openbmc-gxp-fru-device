/*
// Copyright (c) 2021 Hewlett-Packard Development Company, L.P.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/


#include "Utils.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/container/flat_map.hpp>
#include <nlohmann/json.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

extern "C"
{
#include <i2c/smbus.h>
#include <linux/i2c-dev.h>
}

namespace fs = std::filesystem;
static constexpr bool DEBUG = false;

boost::asio::io_service io;
auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
auto objServer = sdbusplus::asio::object_server(systemBus);

const std::string unknown("Unknown");
const std::string eeprom = "/sys/bus/i2c/devices/2-0050/eeprom";

const std::array<std::string, 3> eeproms = {
    "/sys/bus/i2c/devices/2-0055/eeprom",
    "/sys/bus/i2c/devices/2-0054/eeprom",
    "/sys/bus/i2c/devices/2-0050/eeprom"
};

const unsigned int SerialNumberOffset = 1;
const unsigned int SerialNumberSize = 16;
const unsigned int PartNumberOffset = 109;
const unsigned int PartNumberSize = 16;

const unsigned int PcaSerialNumberOffset = 144;
const unsigned int PcaSerialNumberSize = 16;
const unsigned int PcaPartNumberOffset = 160;
const unsigned int PcaPartNumberSize = 16;

const unsigned int MAC0AddressOffset = 132;
const unsigned int MAC1AddressOffset = 138;
const unsigned int MACAddressSize = 6;

std::string GetServerId()
{
    std::string id;
    std::ifstream fin("/sys/class/soc/xreg/server_id");
    if(!fin)
    {
        return unknown;
    }
    getline(fin, id);
    fin.close();

    return id;
}

std::string GetManufactuer()
{
    static const std::string manufacturer("Hewlett Packard Enterprise");
    return manufacturer;
}

std::string GetStr(std::ifstream &fin, const unsigned int offset, const unsigned int size)
{
    if(!fin) {
        return unknown;
    }

    std::vector<char> v(size);
    fin.seekg(offset, std::ios::beg);
    fin.read(v.data(), size);

    return std::string(v.begin(), v.end());
}

std::string GetMACAddress(std::ifstream &fin, const unsigned int offset)
{
    std::vector<char> v(MACAddressSize);
    if(fin) {
        fin.seekg(offset, std::ios::beg);
        fin.read(v.data(), MACAddressSize);
    }

    std::stringstream ss;
    ss << std::setbase(16)<< std::setfill('0') ;
    ss << std::setw(2) << static_cast<int> (v[0]);
    for (int i = 1; i < v.size(); i++) {
        ss << ":" << std::setw(2) << static_cast<int> (v[i]);
    }
    return ss.str();
}

void DumpFRU()
{
    std::cout << "SERVER_ID=" << GetServerId() << std::endl;
    std::cout << "PRODUCT_MANUFACTURER=" << GetManufactuer() << std::endl;

    std::ifstream fin;
    for(std::string eeprom: eeproms) {
        fin.open(eeprom.c_str());
        if(fin.is_open()) {
            std::cout << "PartNumber=" << GetStr(fin, PartNumberOffset, PartNumberSize) << std::endl;
            std::cout << "SerialNumber=" << GetStr(fin, SerialNumberOffset, SerialNumberSize) << std::endl;
            std::cout << "PCAPartNumber=" << GetStr(fin, PcaPartNumberOffset, PcaPartNumberSize) << std::endl;
            std::cout << "PCASerialNumber=" << GetStr(fin, PcaSerialNumberOffset, PcaSerialNumberSize) << std::endl;
            std::cout << "MAC0=" << GetMACAddress(fin, MAC0AddressOffset) << std::endl;
            std::cout << "MAC1=" << GetMACAddress(fin, MAC1AddressOffset) << std::endl;
            fin.close();
            break;
        }
    }
}

void AddFRUObjectToDbus(std::shared_ptr<sdbusplus::asio::dbus_interface>& iface)
{
    std::string productName = "/xyz/openbmc_project/FruDevice/HPE";
    iface = objServer.add_interface(productName, "xyz.openbmc_project.FruDevice");

    iface->register_property("SERVER_ID", GetServerId());
    iface->register_property("PRODUCT_MANUFACTURER", GetManufactuer());

    std::ifstream fin;
    for(std::string eeprom: eeproms) {
        fin.open(eeprom.c_str());
        if(fin.is_open()) {
            iface->register_property("PRODUCT_PART_NUMBER", GetStr(fin, PartNumberOffset, PartNumberSize));
            iface->register_property("PRODUCT_SERIAL_NUMBER", GetStr(fin, SerialNumberOffset, SerialNumberSize));
            iface->register_property("PCA_PART_NUMBER", GetStr(fin, PcaPartNumberOffset, PcaPartNumberSize));
            iface->register_property("PCA_SERIAL_NUMBER", GetStr(fin, PcaSerialNumberOffset, PcaSerialNumberSize));
            iface->register_property("MAC0", GetMACAddress(fin, MAC0AddressOffset));
            iface->register_property("MAC1", GetMACAddress(fin, MAC1AddressOffset));
            fin.close();
            break;
        }
    }

    iface->initialize();
}

void rescanBus(std::shared_ptr<sdbusplus::asio::dbus_interface>& iface)
{
    if(iface != nullptr) {
        objServer.remove_interface(iface);
    }
    AddFRUObjectToDbus(iface);
}

int main()
{
#if 0
    DumpFRU();
#else
    systemBus->request_name("xyz.openbmc_project.GxpFruDevice");

    std::shared_ptr<sdbusplus::asio::dbus_interface> ifaceFruDevice = nullptr;
    std::shared_ptr<sdbusplus::asio::dbus_interface> ifaceFruDeviceManager =
        objServer.add_interface("/xyz/openbmc_project/FruDevice",
                                "xyz.openbmc_project.FruDeviceManager");

    ifaceFruDeviceManager->register_method("ReScan", [&]() {
        rescanBus(ifaceFruDevice);
    });
    ifaceFruDeviceManager->initialize();

    // run the initial scan
    rescanBus(ifaceFruDevice);

    io.run();
#endif

    return 0;
}
