// Note từ Minh Đạt: Bỏ hết mấy cái khai báo ở đầu đi rồi copy paste xuống dưới main.cpp là chạy được
// =====================================================
// DECORATOR MODULE - Dynamic Energy Loads
// =====================================================

// #define DECORATOR_STANDALONE  // ← Bỏ dấu // nếu compile ĐỘC LẬP

#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <iomanip>
#include <cctype>
#include <limits>
#include <nlohmann/json.hpp>
#include <mysql.h>

using namespace std;

// =====================================================
// PHẦN 1: DECORATOR CLASSES
// (Giữ nguyên kiến trúc của nhóm Decorator)
// Không trùng tên với bất kỳ class nào trong main.cpp
// =====================================================

class EnergyComponent {
public:
    virtual ~EnergyComponent() = default;
    virtual void updateStatus() = 0;
    virtual double calculateEnergy(double time) const = 0;
    virtual std::string getDescription() const = 0;
};

class ConcreteDevice : public EnergyComponent {
private:
    std::string name;
    double power;
    bool isON;

public:
    ConcreteDevice(const std::string& n, double p)
        : name(n), power(p), isON(false) {}

    void setStatus(bool status) { isON = status; }
    bool getStatus() const { return isON; }
    double getPower() const { return power; }

    void updateStatus() override {}

    double calculateEnergy(double time) const override {
        if (!isON) return 0.0;
        return (power * time) / 1000.0;
    }

    std::string getDescription() const override {
        return name + (isON ? " [ON]" : " [OFF]");
    }
};

class EnergyDecorator : public EnergyComponent {
protected:
    std::shared_ptr<EnergyComponent> component;

public:
    EnergyDecorator(std::shared_ptr<EnergyComponent> comp) : component(comp) {}

    void updateStatus() override {
        component->updateStatus();
    }

    double calculateEnergy(double time) const override {
        return component->calculateEnergy(time);
    }

    std::string getDescription() const override {
        return component->getDescription();
    }
};

class SmartDeviceController : public EnergyDecorator {
private:
    std::string deviceType;
    int peopleCount;
    double temperature;
    double humidity;
    double lightLux;

public:
    SmartDeviceController(std::shared_ptr<EnergyComponent> comp,
                          std::string type, int people,
                          double temp, double hum, double lux)
        : EnergyDecorator(comp), deviceType(type),
          peopleCount(people), temperature(temp),
          humidity(hum), lightLux(lux) {}

    void updateStatus() override {
        EnergyDecorator::updateStatus();

        auto rawDev = std::dynamic_pointer_cast<ConcreteDevice>(component);
        if (!rawDev) return;

        // Không có người → tắt hết
        if (peopleCount == 0) {
            rawDev->setStatus(false);
            return;
        }

        if (deviceType == "AC") {
            if (peopleCount > 0 && (temperature >= 28.0 || humidity > 75.0)) {
                rawDev->setStatus(true);
            } else if (temperature <= 25.0) {
                rawDev->setStatus(false);
            }
        }
        else if (deviceType == "Light") {
            if (peopleCount > 0 && lightLux < 500.0) {
                rawDev->setStatus(true);
            } else if (lightLux >= 500.0) {
                rawDev->setStatus(false);
            }
        }
        else if (deviceType == "Computer") {
            if (peopleCount > 0) {
                rawDev->setStatus(true);
            }
        }
        // ★ THÊM: Default cho các loại thiết bị khác (Quạt, Máy chiếu...)
        else {
            if (peopleCount > 0) {
                rawDev->setStatus(true);
            } else {
                rawDev->setStatus(false);
            }
        }
    }
};

class AgeDecorator : public EnergyDecorator {
private:
    double wearPercentage;

public:
    AgeDecorator(std::shared_ptr<EnergyComponent> comp, double wearPct)
        : EnergyDecorator(comp), wearPercentage(wearPct) {}

    double calculateEnergy(double time) const override {
        double baseEnergy = EnergyDecorator::calculateEnergy(time);
        double ageFactor = 1.0 + (wearPercentage / 100.0);
        return baseEnergy * ageFactor;
    }

    std::string getDescription() const override {
        return EnergyDecorator::getDescription()
             + " + [Do cu: " + std::to_string((int)wearPercentage) + "%]";
    }
};

// =====================================================
// PHẦN 2: HELPERS CHO TÍCH HỢP DB
// (Mới, không trùng với main.cpp)
// =====================================================

// Map device_type từ JSON (main.cpp) sang loại Decorator hiểu
static std::string mapDecoratorDeviceType(const nlohmann::json& attrs) {
    std::string type = "";
    if (attrs.contains("device_type") && attrs["device_type"].is_string())
        type = attrs["device_type"].get<std::string>();

    // Chuyển về lowercase để match dễ hơn
    std::string lo = type;
    for (char& c : lo) c = (char)std::tolower((unsigned char)c);

    if (lo.find("ac") != std::string::npos ||
        lo.find("dieu hoa") != std::string::npos ||
        lo.find("air") != std::string::npos ||
        lo.find("may lanh") != std::string::npos)
        return "AC";

    if (lo.find("light") != std::string::npos ||
        lo.find("den") != std::string::npos)
        return "Light";

    if (lo.find("computer") != std::string::npos ||
        lo.find("may tinh") != std::string::npos)
        return "Computer";

    return "Other";
}

// Kiểm tra thiết bị có nằm trong lịch hoạt động (schedule) không
static bool decoratorIsOnSchedule(const nlohmann::json& attrs, int hour) {
    if (!attrs.contains("schedule") || !attrs["schedule"].is_array())
        return true; // Không có schedule → luôn được phép

    hour = ((hour % 24) + 24) % 24;
    for (const auto& slot : attrs["schedule"]) {
        if (!slot.is_object()) continue;
        int s = slot.value("start", 0);
        int e = slot.value("end", 0);
        if (s == e) continue;
        if (s < e) { if (hour >= s && hour < e) return true; }
        else       { if (hour >= s || hour < e) return true; }
    }
    return false;
}

// Đọc số từ JSON an toàn
static double decoratorGetNum(const nlohmann::json& a, const std::string& k, double def) {
    if (!a.contains(k)) return def;
    if (a[k].is_number()) return a[k].get<double>();
    return def;
}

// =====================================================
// PHẦN 3: HÀM TÍCH HỢP DB
// Từ main.cpp gọi: runDecoratorSimulation(db.get());
// =====================================================

void runDecoratorSimulation(MYSQL* conn) {
    int buildingId;
    std::cout << "\nNhap ID Toa nha: ";
    std::cin >> buildingId;
    std::cin.ignore(numeric_limits<streamsize>::max(), '\n');

    int hour;
    std::cout << "Nhap gio mo phong (0-23): ";
    std::cin >> hour;
    std::cin.ignore(numeric_limits<streamsize>::max(), '\n');
    hour = (hour < 0) ? 0 : ((hour > 23) ? 23 : hour);

    int people;
    std::cout << "So nguoi trong phong: ";
    std::cin >> people;
    std::cin.ignore(numeric_limits<streamsize>::max(), '\n');

    double temp;
    std::cout << "Nhiet do phong (do C): ";
    std::cin >> temp;
    std::cin.ignore(numeric_limits<streamsize>::max(), '\n');

    double humidity;
    std::cout << "Do am (%): ";
    std::cin >> humidity;
    std::cin.ignore(numeric_limits<streamsize>::max(), '\n');

    double lux;
    std::cout << "Anh sang tu nhien (Lux): ";
    std::cin >> lux;
    std::cin.ignore(numeric_limits<streamsize>::max(), '\n');

    double timeHours = 1.0;

    // Query tất cả device trong tòa nhà
    std::string sql =
        "SELECT d.id, d.device_name, d.attributes, r.name AS room_name "
        "FROM devices d "
        "JOIN rooms r ON d.room_id = r.id "
        "JOIN floors f ON r.floor_id = f.id "
        "WHERE f.building_id = " + std::to_string(buildingId) +
        " ORDER BY f.id, r.id, d.id";

    if (mysql_query(conn, sql.c_str())) {
        std::cout << ">> Loi SQL: " << mysql_error(conn) << "\n";
        return;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        std::cout << ">> Khong co du lieu.\n";
        return;
    }

    // In header
    std::cout << "\n=== DECORATOR SIMULATION tai " << hour << ":00 ===\n";
    std::cout << "Thong so: " << people << " nguoi | " << temp
              << " do C | " << humidity << "% am | " << lux << " lux\n";
    std::cout << string(105, '-') << "\n";
    std::cout << left
              << setw(25) << "Thiet bi"
              << setw(15) << "Phong"
              << setw(10) << "Loai"
              << setw(12) << "Cong suat"
              << setw(14) << "kWh (1 gio)"
              << " | Trang thai Decorator\n";
    std::cout << string(105, '-') << "\n";

    double totalEnergy = 0.0;
    int deviceCount = 0;
    int activeCount = 0;
    MYSQL_ROW row;

    while ((row = mysql_fetch_row(res))) {
        std::string devName  = row[1] ? row[1] : "";
        std::string roomName = row[3] ? row[3] : "";
        std::string attrStr  = row[2] ? row[2] : "{}";

        nlohmann::json attrs = nlohmann::json::parse(attrStr, nullptr, false);
        if (!attrs.is_object()) attrs = nlohmann::json::object();

        double ratedPower = decoratorGetNum(attrs, "rated_power_w", 0.0);
        deviceCount++;

        // Kiểm tra schedule
        if (!decoratorIsOnSchedule(attrs, hour)) {
            std::cout << left
                      << setw(25) << devName.substr(0, 24)
                      << setw(15) << roomName.substr(0, 14)
                      << setw(10) << "-"
                      << setw(12) << ratedPower
                      << setw(14) << "0.000"
                      << " | Ngoai lich (OFF)\n";
            continue;
        }

        // Map loại thiết bị
        std::string devType = mapDecoratorDeviceType(attrs);

        // Tạo chuỗi Decorator: ConcreteDevice → SmartController → AgeDecorator
        auto device = std::make_shared<ConcreteDevice>(devName, ratedPower);
        auto smart  = std::make_shared<SmartDeviceController>(
                          device, devType, people, temp, humidity, lux);

        std::shared_ptr<EnergyComponent> finalComponent = smart;

        // Nếu có độ hao mòn (age_wear_percent) → bọc thêm AgeDecorator
        double wear = decoratorGetNum(attrs, "age_wear_percent", 0.0);
        if (wear > 0.0) {
            finalComponent = std::make_shared<AgeDecorator>(finalComponent, wear);
        }

        // Cho SmartController quyết định bật/tắt
        smart->updateStatus();

        double energy = finalComponent->calculateEnergy(timeHours);
        totalEnergy += energy;

        auto smartDev = std::dynamic_pointer_cast<ConcreteDevice>(device);
        bool isOn = smartDev ? smartDev->getStatus() : false;
        if (isOn) activeCount++;

        std::cout << left
                  << setw(25) << devName.substr(0, 24)
                  << setw(15) << roomName.substr(0, 14)
                  << setw(10) << devType
                  << setw(12) << ratedPower
                  << right << fixed << setprecision(3)
                  << setw(14) << energy
                  << left
                  << " | " << finalComponent->getDescription() << "\n";
    }

    mysql_free_result(res);

    std::cout << string(105, '-') << "\n";
    std::cout << ">> Tong: " << deviceCount << " thiet bi | "
              << activeCount << " BẬT | "
              << fixed << setprecision(3) << totalEnergy
              << " kWh trong 1 gio\n";
    std::cout << ">> Decorator chain: ConcreteDevice -> SmartDeviceController -> AgeDecorator\n";
    std::cout << ">> SmartController: AC (>=28C hoac am>75%) | Light (<500 lux) | Tat khi vang nguoi\n";
}

// =====================================================
// PHẦN 4: STANDALONE MODE
// Chỉ compile khi #define DECORATOR_STANDALONE được bật.
// Khi paste vào main.cpp, phần này tự động bị bỏ qua.
// =====================================================
#ifdef DECORATOR_STANDALONE

static const string DEC_DB_HOST = "127.0.0.1";
static const string DEC_DB_USER = "root";
static const string DEC_DB_PASS = "Minhdat1";
static const string DEC_DB_NAME = "energy_management_db";

// Demo hardcoded (giữ nguyên từ nhóm Decorator)
void runDecoratorDemoHardcoded() {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "=========================================================\n";
    std::cout << "  MO PHONG DIEN NANG TIEU THU ROOM 101 (LUC 14:00)\n";
    std::cout << "=========================================================\n\n";

    int people = 3;
    double temp = 30.0;
    double humidity = 78.0;
    double lux = 300.0;
    double timeInHours = 1.0;

    std::cout << "Thong so moi truong:\n";
    std::cout << "- So nguoi: " << people << " nguoi\n";
    std::cout << "- Nhiet do: " << temp << " degC\n";
    std::cout << "- Do am: " << humidity << " %\n";
    std::cout << "- Anh sang: " << lux << " lux\n";
    std::cout << "---------------------------------------------------------\n\n";

    auto lightDev = std::make_shared<ConcreteDevice>("Light (Den)", 20.0);
    auto acDev    = std::make_shared<ConcreteDevice>("AC (Dieu hoa)", 1200.0);
    auto compDev  = std::make_shared<ConcreteDevice>("Computer (May tinh)", 300.0);

    auto smartLight = std::make_shared<SmartDeviceController>(lightDev, "Light", people, temp, humidity, lux);
    auto smartAC    = std::make_shared<SmartDeviceController>(acDev, "AC", people, temp, humidity, lux);
    auto smartComp  = std::make_shared<SmartDeviceController>(compDev, "Computer", people, temp, humidity, lux);

    std::shared_ptr<EnergyComponent> finalAC = std::make_shared<AgeDecorator>(smartAC, 20.0);

    smartLight->updateStatus();
    smartAC->updateStatus();
    smartComp->updateStatus();

    double eLight = smartLight->calculateEnergy(timeInHours);
    double eAC    = finalAC->calculateEnergy(timeInHours);
    double eComp  = smartComp->calculateEnergy(timeInHours);
    double totalRoomEnergy = eLight + eAC + eComp;

    std::cout << "Ket qua hoat dong & Tieu thu dien nang trong " << timeInHours << " gio:\n\n";
    std::cout << "1. " << smartLight->getDescription() << "\n";
    std::cout << "   -> Dien nang tieu thu: " << eLight << " kWh\n\n";
    std::cout << "2. " << finalAC->getDescription() << "\n";
    std::cout << "   -> Dien nang tieu thu (Da tinh do cu 20%): " << eAC << " kWh\n\n";
    std::cout << "3. " << smartComp->getDescription() << "\n";
    std::cout << "   -> Dien nang tieu thu: " << eComp << " kWh\n\n";
    std::cout << "---------------------------------------------------------\n";
    std::cout << "TONG DIEN NANG ROOM 101: " << totalRoomEnergy << " kWh\n";
    std::cout << "=========================================================\n";
}

int main() {
    std::cout << "=== DECORATOR MODULE (Standalone) ===\n";

    while (true) {
        std::cout << "\n--- DECORATOR MENU ---\n";
        std::cout << "1. Demo hardcoded (Room 101, khong can DB)\n";
        std::cout << "2. Mo phong tu Database\n";
        std::cout << "0. Thoat\n";
        int ch;
        std::cout << "Chon: ";
        std::cin >> ch;
        std::cin.ignore(numeric_limits<streamsize>::max(), '\n');

        if (ch == 0) break;

        if (ch == 1) {
            runDecoratorDemoHardcoded();
        }
        else if (ch == 2) {
            MYSQL* conn = mysql_init(nullptr);
            if (!conn) {
                std::cerr << ">> Loi khoi tao MySQL.\n";
                continue;
            }
            if (!mysql_real_connect(conn, DEC_DB_HOST.c_str(), DEC_DB_USER.c_str(),
                                    DEC_DB_PASS.c_str(), DEC_DB_NAME.c_str(),
                                    3306, nullptr, 0)) {
                std::cerr << ">> Loi ket noi: " << mysql_error(conn) << "\n";
                mysql_close(conn);
                continue;
            }
            mysql_set_character_set(conn, "utf8mb4");
            runDecoratorSimulation(conn);
            mysql_close(conn);
        }
        else {
            std::cout << ">> Khong hop le.\n";
        }
    }
    return 0;
}

#endif // DECORATOR_STANDALONE

