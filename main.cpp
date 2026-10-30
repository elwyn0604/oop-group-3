#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <limits>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <mysql.h>
#include "httplib.h"


using namespace std;
using json = nlohmann::json;

// =====================================================
// MYSQL CONNECTION
// =====================================================
static const string DB_HOST     = "127.0.0.1";
static const string DB_USER     = "root";
static const string DB_PASSWORD = "Minhdat1";
static const string DB_SCHEMA   = "energy_management_db";

// =====================================================
// BASIC INPUT HELPERS
// =====================================================
string getStringInput(const string& prompt) {
    string input;
    cout << prompt;
    getline(cin, input);
    return input;
}

int getIntInput(const string& prompt) {
    int val;
    while (true) {
        cout << prompt;
        if (cin >> val) {
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            return val;
        }
        cin.clear();
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        cout << ">> Du lieu khong hop le. Vui long nhap so nguyen.\n";
    }
}

double getDoubleInput(const string& prompt) {
    double val;
    while (true) {
        cout << prompt;
        if (cin >> val) {
            cin.ignore(numeric_limits<streamsize>::max(), '\n');
            return val;
        }
        cin.clear();
        cin.ignore(numeric_limits<streamsize>::max(), '\n');
        cout << ">> Du lieu khong hop le. Vui long nhap so.\n";
    }
}

int clampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

// =====================================================
// SIMPLE STRING + JSON HELPERS
// =====================================================
string toLowerString(string s) {
    for (char &c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

bool containsAny(string value, const vector<string>& keys) {
    value = toLowerString(value);

    for (const string& k : keys) {
        if (value.find(toLowerString(k)) != string::npos) {
            return true;
        }
    }

    return false;
}

double getJsonNumber(const json& attrs, const string& key, double defaultValue) {
    if (!attrs.contains(key)) return defaultValue;

    if (attrs[key].is_number()) {
        return attrs[key].get<double>();
    }

    if (attrs[key].is_string()) {
        try {
            return stod(attrs[key].get<string>());
        } catch (...) {
            return defaultValue;
        }
    }

    return defaultValue;
}

string getJsonString(const json& attrs, const string& key, const string& defaultValue) {
    if (!attrs.contains(key)) return defaultValue;

    if (attrs[key].is_string()) {
        return attrs[key].get<string>();
    }

    return defaultValue;
}

bool getJsonBool(const json& attrs, const string& key, bool defaultValue) {
    if (!attrs.contains(key)) return defaultValue;

    if (attrs[key].is_boolean()) {
        return attrs[key].get<bool>();
    }

    if (attrs[key].is_number()) {
        return attrs[key].get<int>() != 0;
    }

    if (attrs[key].is_string()) {
        string v = toLowerString(attrs[key].get<string>());
        return (v == "true" || v == "1" || v == "on" || v == "yes");
    }

    return defaultValue;
}

int safeGetInt(const string& s) {
    if (s.empty()) return 0;
    try {
        return stoi(s);
    } catch (...) {
        return 0;
    }
}

double safeGetDouble(const string& s, double defaultValue = 0.0) {
    if (s.empty()) return defaultValue;
    try {
        return stod(s);
    } catch (...) {
        return defaultValue;
    }
}

string normalizeStrategyKey(const string& key) {
    if (key == "eco") return "eco";
    if (key == "peak") return "peak";
    if (key == "auto") return "auto";
    return "normal";
}

// =====================================================
// ROOM STATE + DAY PROFILE
// =====================================================
class RoomState {
public:
    int peopleCount;
    double temperatureC;
    double humidity;
    double lightLux;
    bool hasData;

    RoomState() {
        peopleCount = 0;
        temperatureC = 25.0;
        humidity = 50.0;
        lightLux = 300.0;
        hasData = false;
    }
};

class RoomDayProfile {
public:
    int capacity;
    RoomState hours[24];

    RoomDayProfile() {
        capacity = 10;
    }

    RoomState getStateAt(int hour) const {
        hour = ((hour % 24) + 24) % 24;

        if (hours[hour].hasData) {
            return hours[hour];
        }

        for (int step = 1; step < 24; step++) {
            int h = (hour - step + 24) % 24;
            if (hours[h].hasData) {
                return hours[h];
            }
        }

        return RoomState();
    }
};

// =====================================================
// STRATEGY PATTERN
// =====================================================
class IEnergyStrategy {
public:
    virtual ~IEnergyStrategy() {}

    virtual string getName() = 0;

    virtual double calculateFactor(
        const json& attrs,
        const RoomState& state,
        int capacity,
        int hour
    ) = 0;
};

double occupancyRateOf(const RoomState& state, int capacity) {
    if (capacity <= 0) return 1.0;

    double rate = (double)state.peopleCount / capacity;

    if (rate > 1.0) rate = 1.0;
    if (rate < 0.0) rate = 0.0;

    return rate;
}

class NormalModeStrategy : public IEnergyStrategy {
public:
    string getName() override {
        return "Normal Mode";
    }

    double calculateFactor(
        const json& attrs,
        const RoomState& state,
        int capacity,
        int hour
    ) override {
        return 1.0;
    }
};

class EcoSavingStrategy : public IEnergyStrategy {
public:
    string getName() override {
        return "Eco Saving";
    }

    double calculateFactor(
        const json& attrs,
        const RoomState& state,
        int capacity,
        int hour
    ) override {
        int os = clampInt((int)getJsonNumber(attrs, "occupancy_sensitivity", 0), 0, 5);

        if (os <= 0) return 1.0;

        double occ = occupancyRateOf(state, capacity);
        double reduction = 0.0;

        if (occ <= 0.0) {
            reduction = os * 0.12;
        } else if (occ < 0.3) {
            reduction = os * 0.08;
        } else if (occ < 0.6) {
            reduction = os * 0.04;
        }

        double factor = 1.0 - reduction;

        if (factor < 0.2) factor = 0.2;

        return factor;
    }
};

class PeakShavingStrategy : public IEnergyStrategy {
public:
    string getName() override {
        return "Peak Shaving";
    }

    double calculateFactor(
        const json& attrs,
        const RoomState& state,
        int capacity,
        int hour
    ) override {
        // Peak hours: 18h - 21h
        if (hour < 18 || hour >= 21) {
            return 1.0;
        }

        int ts = clampInt((int)getJsonNumber(attrs, "temp_sensitivity", 0), 0, 5);

        double baseReduction = 0.30;
        double tsKeep = ts * 0.04;
        double reduction = baseReduction - tsKeep;

        if (reduction < 0.05) reduction = 0.05;

        return 1.0 - reduction;
    }
};

class AutoAdaptiveStrategy : public IEnergyStrategy {
public:
    string getName() override {
        return "Auto Adaptive";
    }

    double calculateFactor(
        const json& attrs,
        const RoomState& state,
        int capacity,
        int hour
    ) override {
        int ts = clampInt((int)getJsonNumber(attrs, "temp_sensitivity", 0), 0, 5);
        int os = clampInt((int)getJsonNumber(attrs, "occupancy_sensitivity", 0), 0, 5);
        int hs = clampInt((int)getJsonNumber(attrs, "humidity_sensitivity", 0), 0, 5);

        double factor = 1.0;

        // Occupancy effect
        if (os > 0) {
            double occ = occupancyRateOf(state, capacity);
            double reduction = 0.0;

            if (occ <= 0.0) {
                reduction = os * 0.12;
            } else if (occ < 0.3) {
                reduction = os * 0.08;
            } else if (occ < 0.6) {
                reduction = os * 0.04;
            }

            factor *= (1.0 - reduction);
        }

        // Temperature effect
        if (ts > 0) {
            double temp = state.temperatureC;
            double addition = 0.0;

            if (temp >= 35.0) {
                addition = ts * 0.07;
            } else if (temp >= 30.0) {
                addition = ts * 0.05;
            } else if (temp <= 22.0) {
                addition = -ts * 0.02;
            }

            factor *= (1.0 + addition);
        }

        // Humidity effect
        if (hs > 0 && state.humidity > 60.0) {
            factor *= (1.0 + hs * 0.01);
        }

        // Daylight effect for lights
        string type = getJsonString(attrs, "device_type", "");
        bool daylightSensitive = getJsonBool(attrs, "daylight_sensitive", false);

        if (daylightSensitive && containsAny(type, {"light", "den", "đèn"})) {
            double threshold = getJsonNumber(attrs, "daylight_threshold", 500.0);

            if (state.lightLux >= threshold) {
                factor *= 0.3;
            } else if (state.lightLux >= threshold * 0.5) {
                factor *= 0.6;
            }
        }

        if (factor < 0.1) factor = 0.1;

        return factor;
    }
};

// Global strategy objects
NormalModeStrategy   normalModeStrategy;
EcoSavingStrategy    ecoSavingStrategy;
PeakShavingStrategy  peakShavingStrategy;
AutoAdaptiveStrategy autoAdaptiveStrategy;

string strategyToKey(IEnergyStrategy* s) {
    if (s == &ecoSavingStrategy) return "eco";
    if (s == &peakShavingStrategy) return "peak";
    if (s == &autoAdaptiveStrategy) return "auto";
    return "normal";
}

IEnergyStrategy* keyToStrategy(const string& key) {
    if (key == "eco") return &ecoSavingStrategy;
    if (key == "peak") return &peakShavingStrategy;
    if (key == "auto") return &autoAdaptiveStrategy;
    return &normalModeStrategy;
}

// =====================================================
// COMPOSITE PATTERN
// =====================================================
class IComponent {
public:
    virtual ~IComponent() {}

    virtual int getId() = 0;
    virtual string getName() = 0;
    virtual string getKind() = 0;

    virtual double getDailyKwh() = 0;
    virtual double getWeeklyKwh() = 0;
    virtual double getMonthlyKwh() = 0;
void printTree(int level, int mode) = 0;
    virtual void printDetailDaily() {}

    virtual void setDayProfile(RoomDayProfile* profile) {}
    virtual void setStrategy(IEnergyStrategy* s) {}
};

class CompositeComponent : public IComponent {
protected:
    int id;
    string name;
    vector<IComponent*> children;

public:
    CompositeComponent(int _id, string _name) : id(_id), name(_name) {}

    ~CompositeComponent() {
        for (int i = 0; i < (int)children.size(); i++) {
            delete children[i];
        }
    }

    int getId() override {
        return id;
    }

    string getName() override {
        return name;
    }

    void addChild(IComponent* child) override {
        if (child) {
            children.push_back(child);
        }
    }

    double getDailyKwh() override {
        double total = 0.0;
        for (int i = 0; i < (int)children.size(); i++) {
            total += children[i]->getDailyKwh();
        }
        return total;
    }

    double getWeeklyKwh() override {
        double total = 0.0;
        for (int i = 0; i < (int)children.size(); i++) {
            total += children[i]->getWeeklyKwh();
        }
        return total;
    }

    double getMonthlyKwh() override {
        double total = 0.0;
        for (int i = 0; i < (int)children.size(); i++) {
            total += children[i]->getMonthlyKwh();
        }
        return total;
    }

    void printTree(int level, int mode) override {
        string indent(level * 2, ' ');

        double value;
        string label;

        if (mode == 0) {
            value = getDailyKwh();
            label = "Daily";
        } else if (mode == 1) {
            value = getWeeklyKwh();
            label = "Weekly";
        } else {
            value = getMonthlyKwh();
            label = "Monthly";
        }

        cout << fixed << setprecision(2);
        cout << indent << "[" << getKind() << "] " << name
             << " - " << label << ": " << value << " kWh\n";

        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->printTree(level + 1, mode);
        }
    }

    void printDetailDaily() override {
        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->printDetailDaily();
        }
    }

    void setDayProfile(RoomDayProfile* profile) override {
        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->setDayProfile(profile);
        }
    }

    void setStrategy(IEnergyStrategy* s) override {
        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->setStrategy(s);
        }
    }
};

class Building : public CompositeComponent {
public:
    Building(int id, string code, string name)
        : CompositeComponent(id, name) {}

    string getKind() override {
        return "Building";
    }
};

class Floor : public CompositeComponent {
public:
    Floor(int id, string code, string name)
        : CompositeComponent(id, name) {}

    string getKind() override {
        return "Floor";
    }
};

class Room : public CompositeComponent {
private:
    int capacity;
    RoomDayProfile* profile;
    IEnergyStrategy* strategy;

public:
    Room(int id, string code, string name, int cap)
        : CompositeComponent(id, name), capacity(cap) {
        profile = new RoomDayProfile();
        profile->capacity = cap;
        strategy = &normalModeStrategy;
    }

    ~Room() {
        delete profile;
    }

    string getKind() override {
        return "Room";
    }

    int getCapacity() {
        return capacity;
    }

    RoomDayProfile* getProfile() {
        return profile;
    }

    IEnergyStrategy* getStrategy() {
        return strategy;
    }

    void setRoomStrategy(IEnergyStrategy* s) {
        if (s) strategy = s;
    }

    void setStrategy(IEnergyStrategy* s) override {
        if (s) strategy = s;

        // Pass strategy to devices too
        CompositeComponent::setStrategy(s);
    }

    void printTree(int level, int mode) override {
        string indent(level * 2, ' ');

        double value;
        string label;

        if (mode == 0) {
            value = getDailyKwh();
            label = "Daily";
        } else if (mode == 1) {
            value = getWeeklyKwh();
            label = "Weekly";
        } else {
            value = getMonthlyKwh();
            label = "Monthly";
        }

        cout << fixed << setprecision(2);
        cout << indent << "[Room] " << name
             << " (suc chua: " << capacity << ")"
             << " [Mode: " << strategy->getName() << "]"
             << " - " << label << ": " << value << " kWh\n";

        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->printTree(level + 1, mode);
        }
    }

    void applyProfileToChildren() {
        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->setDayProfile(profile);
            children[i]->setStrategy(strategy);
        }
    }
};

class Device : public IComponent {
private:
    int id;
    string name;
    json attributes;

    IEnergyStrategy* strategy;
    RoomDayProfile* profile;

    struct ScheduleSlot {
        int startHour;
        int endHour;
    };

    vector<ScheduleSlot> schedule;

    double ratedPower;
    double usageDaysPerWeek;

public:
    Device(int _id, string _name, const string& attrJson)
        : id(_id), name(_name), profile(nullptr) {
        if (!attrJson.empty()) {
            attributes = json::parse(attrJson, nullptr, false);
            if (!attributes.is_object()) {
                attributes = json::object();
            }
        } else {
            attributes = json::object();
        }

        ratedPower = getJsonNumber(attributes, "rated_power_w", 0.0);
        usageDaysPerWeek = getJsonNumber(attributes, "usage_days_per_week", 7.0);

        if (usageDaysPerWeek > 7) usageDaysPerWeek = 7;
        if (usageDaysPerWeek < 0) usageDaysPerWeek = 0;

        strategy = &normalModeStrategy;

        if (attributes.contains("schedule") && attributes["schedule"].is_array()) {
            for (const auto& slot : attributes["schedule"]) {
                if (!slot.is_object()) continue;

                ScheduleSlot s;
                s.startHour = slot.value("start", 0);
                s.endHour = slot.value("end", 0);
                schedule.push_back(s);
            }
        }
    }

    int getId() override {
        return id;
    }

    string getName() override {
        return name;
    }

    string getKind() override {
        return "Device";
    }     // Ham lay loai thiet bi (light, ac, fan...) de Decorator su dung
    string getDeviceType() {
        return getJsonString(attributes, "device_type", "");
    }

    void setDayProfile(RoomDayProfile* p) override {
        profile = p;
    }

    void setStrategy(IEnergyStrategy* s) override {
        if (s) strategy = s;
    }

    bool isOnAt(int hour) const {
        hour = ((hour % 24) + 24) % 24;

        for (int i = 0; i < (int)schedule.size(); i++) {
            int s = schedule[i].startHour;
            int e = schedule[i].endHour;

            if (s == e) continue;

            if (s < e) {
                if (hour >= s && hour < e) return true;
            } else {
                if (hour >= s || hour < e) return true;
            }
        }

        return false;
    }

    double getDailyKwh() override {
        double total = 0.0;

        for (int hour = 0; hour < 24; hour++) {
    double getMonthlyKwh() override {
        return getWeeklyKwh() * 52.0 / 12.0;
    }

        double getKwhAtHour(int hour) override {
        if (!isOnAt(hour)) return 0.0;

        RoomState state = profile ? profile->getStateAt(hour) : RoomState();
        int cap = profile ? profile->capacity : 10;

        double factor = strategy ? strategy->calculateFactor(attributes, state, cap, hour) : 1.0;

        return (ratedPower / 1000.0) * 1.0 * factor;
    }

void printTree(int level, int mode) override {
        string indent(level * 2, ' ');

        double value;
        string label;

        if (mode == 0) {
            value = getDailyKwh();
            label = "Daily";
        } else if (mode == 1) {
            value = getWeeklyKwh();
            label = "Weekly";
        } else {
            value = getMonthlyKwh();
            label = "Monthly";
        }

        cout << fixed << setprecision(2);
        cout << indent << "[Device] " << name
             << " (" << strategy->getName() << ")"
             << " - " << label << ": " << value << " kWh\n";
    }

    void printDetailDaily() override {
        cout << fixed << setprecision(2);
        cout << "\n[Chi tiet theo gio] " << name
             << " - Strategy: " << strategy->getName() << "\n";

        double dayTotal = 0.0;
        bool hasOnHour = false;

        for (int hour = 0; hour < 24; hour++) {
            if (!isOnAt(hour)) continue;

            hasOnHour = true;

            RoomState state = profile ? profile->getStateAt(hour) : RoomState();
            int cap = profile ? profile->capacity : 10;

            double factor = strategy->calculateFactor(attributes, state, cap, hour);
            double kwh = (ratedPower / 1000.0) * 1.0 * factor;

            dayTotal += kwh;

            cout << "    " << setw(2) << hour << "h: BAT "
                 << " | " << state.peopleCount << " nguoi"
                 << " | " << state.temperatureC << " do C"
                 << " | do am " << state.humidity << "%"
                 << " | lux " << state.lightLux
                 << " | he so " << factor
                 << " | " << kwh << " kWh\n";
        }

        if (!hasOnHour) {
            cout << "    (Thiet bi khong bat gio nao trong ngay)\n";
        } else {
            cout << "    Tong ngay: " << dayTotal << " kWh\n";
        }
    }
};

// =====================================================
// DECORATOR PATTERN  (Dynamic Energy Loads)
// -----------------------------------------------------
// Y tuong: "bao boc" (wrap) mot Device bang cac lop Decorator.
// Moi Decorator them 1 luat dieu chinh dien nang theo moi truong,
// ma KHONG can sua lop Device goc.
//
//   Device  <-- OccupancyLoadDecorator <-- TemperatureLoadDecorator <-- DaylightLoadDecorator
//
// Day la da hinh (polymorphism): Room chi thay IComponent*,
// no khong can biet ben trong la Device thuong hay Device da duoc bao boc.
// =====================================================

// Lop Decorator goc: giu con tro toi doi tuong bi bao boc
class EnergyDecorator : public IComponent {
protected:
    IComponent* component;     // doi tuong bi bao boc
    RoomDayProfile* profile;   // du lieu moi truong cua phong (giong Device)
    string deviceType;         // loai thiet bi: light / ac / fan / ...

public:
    EnergyDecorator(IComponent* c, string devType) {
        component = c;
        deviceType = devType;
        profile = nullptr;
    }

    // Decorator nam giu component -> phai huy no khi bi huy
    virtual ~EnergyDecorator() {
        delete component;
    }

    // 3 ham nay chi "hoi giup" component ben trong
    int getId() override      { return component->getId(); }
    string getName() override { return component->getName(); }
    string getKind() override { return component->getKind(); }

    // Nhan du lieu tu Room, roi truyen tiep xuong component ben trong
    void setDayProfile(RoomDayProfile* p) override {
        profile = p;
        component->setDayProfile(p);
    }

    void setStrategy(IEnergyStrategy* s) override {
        component->setStrategy(s);
    }

    // MAC DINH: tra ve ket qua cua component ben trong.
    // Cac Decorator con se ghi de (override) ham nay de dieu chinh.
    double getKwhAtHour(int hour) override {
        return component->getKwhAtHour(hour);
    }

    // Tinh lai tong ngay theo tung gio DA duoc dieu chinh
    double getDailyKwh() override {
        double total = 0.0;
        for (int hour = 0; hour < 24; hour++) {
            total += getKwhAtHour(hour);
        }
        return total;
    }

    // Tuan / Thang: giu dung "so ngay hoat dong" cua Device,
    // chi thay doi theo dung ty le ma Decorator da giam/tang trong ngay
    double getWeeklyKwh() override {
        double baseDaily = component->getDailyKwh();
        if (baseDaily <= 0.0) return component->getWeeklyKwh();
        double ratio = getDailyKwh() / baseDaily;
        return component->getWeeklyKwh() * ratio;
    }

    double getMonthlyKwh() override {
        double baseDaily = component->getDailyKwh();
        if (baseDaily <= 0.0) return component->getMonthlyKwh();
        double ratio = getDailyKwh() / baseDaily;
        return component->getMonthlyKwh() * ratio;
    }

    double getKwhAtHour(int hour) override {
        double total = 0.0;
        for (size_t i = 0; i < children.size(); i++) {
            total += children[i]->getKwhAtHour(hour);
        }
        return total;
    }

    void printTree(int level, int mode) override {
        string indent(level * 2, ' ');
        double value;
        string label;
        if (mode == 0)      { value = getDailyKwh();   label = "Daily"; }
        else if (mode == 1) { value = getWeeklyKwh();  label = "Weekly"; }
        else                { value = getMonthlyKwh(); label = "Monthly"; }

        cout << fixed << setprecision(2);
        cout << indent << "[" << getKind() << " +Decorator] " << getName()
             << " - " << label << ": " << value << " kWh\n";
    }

    void printDetailDaily() override {
        cout << fixed << setprecision(2);
        cout << "\n[Chi tiet theo gio - Decorator] " << getName() << "\n";
        double dayTotal = 0.0;
        bool hasOnHour = false;
        for (int hour = 0; hour < 24; hour++) {
            double kwh = getKwhAtHour(hour);
            if (kwh <= 0.0) continue;
            hasOnHour = true;
            dayTotal += kwh;
            cout << "    " << setw(2) << hour << "h: " << kwh << " kWh\n";
        }
        if (!hasOnHour) cout << "    (Thiet bi khong ton dien gio nao)\n";
        else            cout << "    Tong ngay: " << dayTotal << " kWh\n";
    }
};

// -----------------------------------------------------
// Decorator 1: Phong vang nguoi -> giam / tat thiet bi
// -----------------------------------------------------
class OccupancyLoadDecorator : public EnergyDecorator {
public:
    OccupancyLoadDecorator(IComponent* c, string devType) : EnergyDecorator(c, devType) {}

// -----------------------------------------------------
// Decorator 2: Nhiet do cao -> dieu hoa ton dien hon
// -----------------------------------------------------
class TemperatureLoadDecorator : public EnergyDecorator {
public:
    TemperatureLoadDecorator(IComponent* c, string devType) : EnergyDecorator(c, devType) {}

// -----------------------------------------------------
// Decorator 3: Du anh sang tu nhien -> den giam dien
// -----------------------------------------------------
class DaylightLoadDecorator : public EnergyDecorator {
public:
    DaylightLoadDecorator(IComponent* c, string devType) : EnergyDecorator(c, devType) {}

// =====================================================
// MYSQL CONTEXT
// =====================================================
class DbContext {
private:
    MYSQL* conn;

public:
    DbContext() {
        conn = mysql_init(nullptr);

        if (!mysql_real_connect(
            conn,
            DB_HOST.c_str(),
            DB_USER.c_str(),
            DB_PASSWORD.c_str(),
            DB_SCHEMA.c_str(),
            3306,
            nullptr,
            0
        )) {
            throw runtime_error("Loi ket noi MySQL: " + string(mysql_error(conn)));
        }

        mysql_set_character_set(conn, "utf8mb4");
    }

    ~DbContext() {
        if (conn) mysql_close(conn);
    }

    MYSQL* get() {
        return conn;
    }

    string escape(const string& s) {
        string out(s.size() * 2 + 1, '\0');

        unsigned long len = mysql_real_escape_string(
            conn,
            &out[0],
            s.c_str(),
            s.length()
        );

        out.resize(len);
        return out;
    }
};

vector<vector<string>> executeQuery(MYSQL* conn, const string& sql) {
    if (mysql_query(conn, sql.c_str())) {
        throw runtime_error("Loi SQL: " + string(mysql_error(conn)));
    }

    MYSQL_RES* result = mysql_store_result(conn);

    if (!result) {
        return {};
    }

    vector<vector<string>> rows;

    int num_fields = mysql_num_fields(result);
    MYSQL_ROW row;

    while ((row = mysql_fetch_row(result))) {
        vector<string> row_data;

        for (int i = 0; i < num_fields; i++) {
            row_data.push_back(row[i] ? string(row[i]) : "");
        }

        rows.push_back(row_data);
    }

    mysql_free_result(result);

    return rows;
}

// =====================================================
// LOAD BUILDING FROM DATABASE
// =====================================================
Building* loadBuildingFromDB(DbContext& db, int buildingId) {
    string sqlB =
        "SELECT id, building_code, name "
        "FROM buildings "
        "WHERE id = " + to_string(buildingId) +
        " AND status = 'active'";

    auto b_rows = executeQuery(db.get(), sqlB);

    if (b_rows.empty()) return nullptr;

    Building* building = new Building(
        safeGetInt(b_rows[0][0]),
        b_rows[0][1],
        b_rows[0][2]
    );

    string sqlF =
        "SELECT id, floor_code, name "
        "FROM floors "
        "WHERE building_id = " + to_string(buildingId) +
        " AND status = 'active'";

    auto f_rows = executeQuery(db.get(), sqlF);

    for (const auto& f : f_rows) {
        int floorId = safeGetInt(f[0]);

        Floor* floor = new Floor(floorId, f[1], f[2]);

        string sqlR =
            "SELECT id, room_code, name, capacity, energy_strategy "
            "FROM rooms "
            "WHERE floor_id = " + to_string(floorId) +
            " AND status = 'active'";

        auto r_rows = executeQuery(db.get(), sqlR);

        for (const auto& r : r_rows) {
            int roomId = safeGetInt(r[0]);
            string roomCode = r[1];
            string roomName = r[2];
            int capacity = safeGetInt(r[3]);
            string stratKey = r[4];

            if (capacity <= 0) capacity = 10;

            Room* room = new Room(roomId, roomCode, roomName, capacity);
            room->setRoomStrategy(keyToStrategy(normalizeStrategyKey(stratKey)));

            string sqlS =
                "SELECT hour_of_day, people_count, temperature_c, humidity, light_lux "
                "FROM room_states "
                "WHERE room_id = " + to_string(roomId);

            auto s_rows = executeQuery(db.get(), sqlS);

            RoomDayProfile* profile = room->getProfile();

            for (const auto& srow : s_rows) {
                int h = safeGetInt(srow[0]);

                if (h < 0 || h > 23) continue;

                profile->hours[h].peopleCount = safeGetInt(srow[1]);
                profile->hours[h].temperatureC = safeGetDouble(srow[2], 25.0);
                profile->hours[h].humidity = safeGetDouble(srow[3], 50.0);
                profile->hours[h].lightLux = safeGetDouble(srow[4], 300.0);
                profile->hours[h].hasData = true;
            }

            string sqlD =
                "SELECT id, device_name, attributes "
                "FROM devices "
                "WHERE room_id = " + to_string(roomId) +
                " AND status = 'active'";

            auto d_rows = executeQuery(db.get(), sqlD);

            for (const auto& d : d_rows) {
                Device* device = new Device(
                    safeGetInt(d[0]),
                    d[1],
                    d[2]
                );

                string devType = device->getDeviceType();

                // DECORATOR PATTERN: bao boc Device bang 3 Decorator
                IComponent* finalDevice = device;
                finalDevice = new OccupancyLoadDecorator(finalDevice, devType);
                finalDevice = new TemperatureLoadDecorator(finalDevice, devType);
                finalDevice = new DaylightLoadDecorator(finalDevice, devType);

                room->addChild(finalDevice);
            }

            room->applyProfileToChildren();

            floor->addChild(room);
        }

        building->addChild(floor);
    }

    return building;
}

// =====================================================
// INSERT / UPDATE FUNCTIONS
// =====================================================
int insertBuilding(DbContext& db, const string& code, const string& name) {
    string sql =
        "INSERT INTO buildings (building_code, name) VALUES ('" +
        db.escape(code) + "', '" + db.escape(name) + "')";

    if (mysql_query(db.get(), sql.c_str())) return -1;

    return (int)mysql_insert_id(db.get());
}

int insertFloor(DbContext& db, int b_id, const string& code, const string& name) {
    string sql =
        "INSERT INTO floors (building_id, floor_code, name) VALUES (" +
        to_string(b_id) + ", '" + db.escape(code) + "', '" + db.escape(name) + "')";

    if (mysql_query(db.get(), sql.c_str())) return -1;

    return (int)mysql_insert_id(db.get());
}

int insertRoom(DbContext& db, int f_id, const string& code, const string& name, int capacity) {
    string sql =
        "INSERT INTO rooms (floor_id, room_code, name, capacity) VALUES (" +
        to_string(f_id) + ", '" + db.escape(code) + "', '" + db.escape(name) + "', " +
        to_string(capacity) + ")";

    if (mysql_query(db.get(), sql.c_str())) return -1;

    return (int)mysql_insert_id(db.get());
}

int insertDevice(DbContext& db, int r_id, const string& name, const json& attrs) {
    string sql =
        "INSERT INTO devices (room_id, device_name, attributes) VALUES (" +
        to_string(r_id) + ", '" + db.escape(name) + "', '" + db.escape(attrs.dump()) + "')";

    if (mysql_query(db.get(), sql.c_str())) return -1;

    return (int)mysql_insert_id(db.get());
}

int upsertRoomStates(
    DbContext& db,
    int roomId,
    int startHour,
    int endHour,
    int people,
    double temperature,
    double humidity,
    double lightLux
) {
    int successCount = 0;

    for (int h = startHour; h <= endHour; h++) {
        if (h < 0 || h > 23) continue;

        string sql =
            "INSERT INTO room_states "
            "(room_id, hour_of_day, people_count, temperature_c, humidity, light_lux) VALUES (" +
            to_string(roomId) + ", " +
            to_string(h) + ", " +
            to_string(people) + ", " +
            to_string(temperature) + ", " +
            to_string(humidity) + ", " +
            to_string(lightLux) +
            ") ON DUPLICATE KEY UPDATE "
            "people_count = VALUES(people_count), "
            "temperature_c = VALUES(temperature_c), "
            "humidity = VALUES(humidity), "
            "light_lux = VALUES(light_lux)";

        if (mysql_query(db.get(), sql.c_str()) == 0) {
            successCount++;
        }
    }

    return successCount;
}

bool updateRoomStrategy(DbContext& db, int roomId, const string& stratKey) {
    string key = normalizeStrategyKey(stratKey);

    string sql =
        "UPDATE rooms SET energy_strategy = '" + db.escape(key) + "' "
        "WHERE id = " + to_string(roomId);

    return mysql_query(db.get(), sql.c_str()) == 0;
}

// =====================================================
// LIST FLOORS / ROOMS FOR CLI
// =====================================================
void listFloors(DbContext& db) {
    auto rows = executeQuery(
        db.get(),
        "SELECT f.id, f.floor_code, f.name, b.name "
        "FROM floors f "
        "JOIN buildings b ON f.building_id = b.id "
        "ORDER BY f.id"
    );

    if (rows.empty()) {
        cout << ">> Chua co Tang nao.\n";
        return;
    }

    cout << "\n--- DANH SACH CAC TANG ---\n";
    cout << "ID | Ma tang | Ten tang | Thuoc toa nha\n";

    for (const auto& r : rows) {
        cout << r[0] << " | " << r[1] << " | " << r[2] << " | " << r[3] << "\n";
    }
}

void listRooms(DbContext& db) {
    auto rows = executeQuery(
        db.get(),
        "SELECT r.id, r.room_code, r.name, f.name, r.capacity, r.energy_strategy "
        "FROM rooms r "
        "JOIN floors f ON r.floor_id = f.id "
        "ORDER BY r.id"
    );

    if (rows.empty()) {
        cout << ">> Chua co Phong nao.\n";
        return;
    }

    cout << "\n--- DANH SACH CAC PHONG ---\n";
    cout << "ID | Ma phong | Ten phong | Thuoc tang | Suc chua | Che do\n";

    for (const auto& r : rows) {
        cout << r[0] << " | " << r[1] << " | " << r[2] << " | "
             << r[3] << " | " << r[4] << " | " << r[5] << "\n";
    }
}


// Helper functions for HTTP API
inline json parseJsonBody(const httplib::Request& req) {
    try {
        if (req.body.empty()) return json::object();
        return json::parse(req.body);
    } catch (...) {
        return json::object();
    }
}

inline int bodyInt(const json& body, const string& key, int def) {
    if (!body.contains(key)) return def;
    if (body[key].is_number_integer()) return body[key].get<int>();
    if (body[key].is_number()) return (int)body[key].get<double>();
    if (body[key].is_string()) {
        try { return stoi(body[key].get<string>()); } catch (...) { return def; }
    }
    return def;
}

inline double bodyDouble(const json& body, const string& key, double def) {
    if (!body.contains(key)) return def;
    if (body[key].is_number()) return body[key].get<double>();
    if (body[key].is_string()) {
        try { return stod(body[key].get<string>()); } catch (...) { return def; }
    }
    return def;
}

inline string bodyString(const json& body, const string& key, const string& def) {
    if (!body.contains(key)) return def;
    if (body[key].is_string()) return body[key].get<string>();
    return def;
}


// =====================================================
// HTTP API SERVER
// =====================================================
void startHttpServer() {
    httplib::Server svr;

    // Serve static files, including index.html
    svr.set_mount_point("/", ".");

    // Optional CORS preflight support
    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        res.status = 204;
    });

    // ------------------------------------------------------------
    // HEALTH
    // ------------------------------------------------------------
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        try {
            DbContext db;

            auto rows = executeQuery(db.get(), "SELECT COUNT(*) FROM buildings");

            int buildingCount = 0;

            if (!rows.empty() && !rows[0].empty()) {
                buildingCount = safeGetInt(rows[0][0]);
            }

            json out;
            out["status"] = "ok";
            out["database"] = "connected";
            out["buildingCount"] = buildingCount;
            out["server"] = "EMS C++ backend";

            res.set_content(out.dump(), "application/json");
        } catch (const exception& e) {
            json out;
            out["status"] = "error";
            out["database"] = "disconnected";
            out["error"] = e.what();

            res.status = 500;
            res.set_content(out.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // GET BUILDINGS
    // ------------------------------------------------------------
    svr.Get("/api/buildings", [](const httplib::Request&, httplib::Response& res) {
        try {
            DbContext db;

            auto rows = executeQuery(
                db.get(),
                "SELECT id, building_code, name "
                "FROM buildings "
                "WHERE status = 'active' "
                "ORDER BY id"
            );

            json arr = json::array();

            for (const auto& r : rows) {
                json obj;
                obj["id"] = safeGetInt(r[0]);
                obj["code"] = r[1];
                obj["name"] = r[2];

                arr.push_back(obj);
            }

            res.set_content(arr.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // GET BUILDING TOTAL + HOURLY PROFILE
    // ------------------------------------------------------------
    svr.Get(R"(/api/building/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            int buildingId = stoi(req.matches[1]);

            DbContext db;

            Building* building = loadBuildingFromDB(db, buildingId);

            if (!building) {
                res.status = 404;
                res.set_content(json{{"error", "Building not found"}}.dump(), "application/json");
                return;
            }

            json result;
            result["building_id"] = buildingId;
            result["building_name"] = building->getName();
            result["daily_kwh"] = building->getDailyKwh();
            result["weekly_kwh"] = building->getWeeklyKwh();
            result["monthly_kwh"] = building->getMonthlyKwh();

            json hourly = json::array();

            for (int h = 0; h < 24; h++) {
                hourly.push_back(building->getKwhAtHour(h));
            }

            result["hourly_kwh"] = hourly;

            delete building;

            res.set_content(result.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // GET FLOORS
    // ------------------------------------------------------------
    svr.Get("/api/floors", [](const httplib::Request& req, httplib::Response& res) {
        try {
            DbContext db;

            string buildingId = req.get_param_value("building_id");

            string sql =
                "SELECT f.id, f.floor_code, f.name, b.name "
                "FROM floors f "
                "JOIN buildings b ON f.building_id = b.id "
                "WHERE f.status = 'active' ";

            if (!buildingId.empty()) {
                sql += " AND f.building_id = " + to_string(safeGetInt(buildingId));
            }

            sql += " ORDER BY f.id";

            auto rows = executeQuery(db.get(), sql);

            json arr = json::array();

            for (const auto& r : rows) {
                json obj;
                obj["id"] = safeGetInt(r[0]);
                obj["code"] = r[1];
                obj["name"] = r[2];
                obj["building"] = r[3];

                arr.push_back(obj);
            }

            res.set_content(arr.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // GET ROOMS
    // ------------------------------------------------------------
    svr.Get("/api/rooms", [](const httplib::Request& req, httplib::Response& res) {
        try {
            DbContext db;

            string buildingId = req.get_param_value("building_id");
            string floorId = req.get_param_value("floor_id");

            string sql =
                "SELECT r.id, r.room_code, r.name, f.name, r.capacity, r.energy_strategy "
                "FROM rooms r "
                "JOIN floors f ON r.floor_id = f.id "
                "WHERE r.status = 'active' AND f.status = 'active' ";

            if (!buildingId.empty()) {
                sql += " AND f.building_id = " + to_string(safeGetInt(buildingId));
            }

            if (!floorId.empty()) {
                sql += " AND r.floor_id = " + to_string(safeGetInt(floorId));
            }

            sql += " ORDER BY r.id";

            auto rows = executeQuery(db.get(), sql);

            json arr = json::array();

            for (const auto& r : rows) {
                json obj;
                obj["id"] = safeGetInt(r[0]);
                obj["code"] = r[1];
                obj["name"] = r[2];
                obj["floor"] = r[3];
                obj["capacity"] = safeGetInt(r[4]);
                obj["strategy"] = r[5];

                arr.push_back(obj);
            }

            res.set_content(arr.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // GET DEVICES
    // ------------------------------------------------------------
    svr.Get("/api/devices", [](const httplib::Request& req, httplib::Response& res) {
        try {
            string roomId = req.get_param_value("room_id");

            if (roomId.empty()) {
                res.status = 400;
                res.set_content(
                    json{{"error", "room_id is required"}}.dump(),
                    "application/json"
                );
                return;
            }

            DbContext db;

            string sql =
                "SELECT d.id, d.device_name, d.status, d.attributes, r.name "
                "FROM devices d "
                "JOIN rooms r ON d.room_id = r.id "
                "WHERE d.room_id = " + to_string(safeGetInt(roomId)) +
                " ORDER BY d.id";

            auto rows = executeQuery(db.get(), sql);

            json arr = json::array();

            for (const auto& r : rows) {
                json obj;
                obj["id"] = safeGetInt(r[0]);
                obj["name"] = r[1];
                obj["status"] = r[2];
                obj["room"] = r[4];

                json attrs = json::parse(r[3], nullptr, false);

                if (attrs.is_object()) {
                    obj["attributes"] = attrs;
                } else {
                    obj["attributes"] = json::object();
                }

                arr.push_back(obj);
            }

            res.set_content(arr.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST BUILDING
    // ------------------------------------------------------------
    svr.Post("/api/buildings", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            string code = bodyString(body, "code", "");
            string name = bodyString(body, "name", "");

            if (code.empty() || name.empty()) {
                res.status = 400;
                res.set_content(
                    json{{"error", "code and name are required"}}.dump(),
                    "application/json"
                );
                return;
            }

            DbContext db;

            int id = insertBuilding(db, code, name);

            if (id <= 0) {
                res.status = 500;
                res.set_content(
                    json{{"error", "Cannot insert building"}}.dump(),
                    "application/json"
                );
                return;
            }

            res.set_content(
                json{{"success", true}, {"id", id}}.dump(),
                "application/json"
            );
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST FLOOR
    // ------------------------------------------------------------
    svr.Post("/api/floors", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            int buildingId = bodyInt(body, "building_id", 0);
            string code = bodyString(body, "code", "");
            string name = bodyString(body, "name", "");

            if (buildingId <= 0 || code.empty() || name.empty()) {
                res.status = 400;
                res.set_content(
                    json{{"error", "building_id, code, name are required"}}.dump(),
                    "application/json"
                );
                return;
            }

            DbContext db;

            int id = insertFloor(db, buildingId, code, name);

            if (id <= 0) {
                res.status = 500;
                res.set_content(
                    json{{"error", "Cannot insert floor"}}.dump(),
                    "application/json"
                );
                return;
            }

            res.set_content(
                json{{"success", true}, {"id", id}}.dump(),
                "application/json"
            );
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST ROOM
    // ------------------------------------------------------------
    svr.Post("/api/rooms", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            int floorId = bodyInt(body, "floor_id", 0);
            string code = bodyString(body, "code", "");
            string name = bodyString(body, "name", "");
            int capacity = bodyInt(body, "capacity", 10);

            if (floorId <= 0 || code.empty() || name.empty()) {
                res.status = 400;
                res.set_content(
                    json{{"error", "floor_id, code, name are required"}}.dump(),
                    "application/json"
                );
                return;
            }

            if (capacity <= 0) capacity = 10;

            DbContext db;

            int id = insertRoom(db, floorId, code, name, capacity);

            if (id <= 0) {
                res.status = 500;
                res.set_content(
                    json{{"error", "Cannot insert room"}}.dump(),
                    "application/json"
                );
                return;
            }

            res.set_content(
                json{{"success", true}, {"id", id}}.dump(),
                "application/json"
            );
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST DEVICE
    // ------------------------------------------------------------
    svr.Post("/api/devices", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            int roomId = bodyInt(body, "room_id", 0);
            string deviceName = bodyString(body, "device_name", "");

            json attributes = json::object();

            if (body.contains("attributes") && body["attributes"].is_object()) {
                attributes = body["attributes"];
            }

            if (roomId <= 0 || deviceName.empty()) {
                res.status = 400;
                res.set_content(
                    json{{"error", "room_id and device_name are required"}}.dump(),
                    "application/json"
                );
                return;
            }

            DbContext db;

            int id = insertDevice(db, roomId, deviceName, attributes);

            if (id <= 0) {
                res.status = 500;
                res.set_content(
                    json{{"error", "Cannot insert device"}}.dump(),
                    "application/json"
                );
                return;
            }

            res.set_content(
                json{{"success", true}, {"id", id}}.dump(),
                "application/json"
            );
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });


    // ------------------------------------------------------------
    // DELETE BUILDING
    // ------------------------------------------------------------
    svr.Delete(R"(/api/buildings/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            int id = stoi(req.matches[1]);
            DbContext db;
            string sql = "DELETE FROM buildings WHERE id = " + to_string(id);
            if (mysql_query(db.get(), sql.c_str())) {
                res.status = 500;
                res.set_content(json{{"error", mysql_error(db.get())}}.dump(), "application/json");
                return;
            }
            res.set_content(json{{"success", true}}.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // DELETE FLOOR
    // ------------------------------------------------------------
    svr.Delete(R"(/api/floors/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            int id = stoi(req.matches[1]);
            DbContext db;
            string sql = "DELETE FROM floors WHERE id = " + to_string(id);
            if (mysql_query(db.get(), sql.c_str())) {
                res.status = 500;
                res.set_content(json{{"error", mysql_error(db.get())}}.dump(), "application/json");
                return;
            }
            res.set_content(json{{"success", true}}.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // DELETE ROOM
    // ------------------------------------------------------------
    svr.Delete(R"(/api/rooms/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            int id = stoi(req.matches[1]);
            DbContext db;
            string sql = "DELETE FROM rooms WHERE id = " + to_string(id);
            if (mysql_query(db.get(), sql.c_str())) {
                res.status = 500;
                res.set_content(json{{"error", mysql_error(db.get())}}.dump(), "application/json");
                return;
            }
            res.set_content(json{{"success", true}}.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // DELETE DEVICE
    // ------------------------------------------------------------
    svr.Delete(R"(/api/devices/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            int id = stoi(req.matches[1]);
            DbContext db;
            string sql = "DELETE FROM devices WHERE id = " + to_string(id);
            if (mysql_query(db.get(), sql.c_str())) {
                res.status = 500;
                res.set_content(json{{"error", mysql_error(db.get())}}.dump(), "application/json");
                return;
            }
            res.set_content(json{{"success", true}}.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST ROOM STATES
    // ------------------------------------------------------------
    svr.Post("/api/room-states", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            int roomId = bodyInt(body, "room_id", 0);
            int startHour = clampInt(bodyInt(body, "start_hour", 0), 0, 23);
            int endHour = clampInt(bodyInt(body, "end_hour", 0), 0, 23);
            int people = max(0, bodyInt(body, "people_count", 0));
            double temp = bodyDouble(body, "temperature_c", 25.0);
            double humidity = bodyDouble(body, "humidity", 50.0);
            double lux = bodyDouble(body, "light_lux", 300.0);

            if (roomId <= 0) {
                res.status = 400;
                res.set_content(
                    json{{"error", "room_id is required"}}.dump(),
                    "application/json"
                );
                return;
            }

            if (endHour < startHour) {
                swap(startHour, endHour);
            }

            DbContext db;

            int updated = upsertRoomStates(
                db,
                roomId,
                startHour,
                endHour,
                people,
                temp,
                humidity,
                lux
            );

            res.set_content(
                json{
                    {"success", true},
                    {"room_id", roomId},
                    {"updated_hours", updated}
                }.dump(),
                "application/json"
            );
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST ROOM STRATEGY
    // ------------------------------------------------------------
    svr.Post("/api/strategy/room", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            int roomId = bodyInt(body, "roomId", 0);
            string strategy = normalizeStrategyKey(bodyString(body, "strategy", "normal"));

            if (roomId <= 0) {
                res.status = 400;
                res.set_content(
                    json{{"error", "roomId is required"}}.dump(),
                    "application/json"
                );
                return;
            }

            DbContext db;

            bool ok = updateRoomStrategy(db, roomId, strategy);

            if (!ok) {
                res.status = 500;
                res.set_content(
                    json{{"error", "Cannot update room strategy"}}.dump(),
                    "application/json"
                );
                return;
            }

            res.set_content(
                json{
                    {"success", true},
                    {"roomId", roomId},
                    {"strategy", strategy}
                }.dump(),
                "application/json"
            );
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST BUILDING STRATEGY
    // Updates all rooms in building
    // ------------------------------------------------------------
    svr.Post("/api/strategy/building", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            int buildingId = bodyInt(body, "buildingId", 0);
            string strategy = normalizeStrategyKey(bodyString(body, "strategy", "normal"));

            if (buildingId <= 0) {
                res.status = 400;
                res.set_content(
                    json{{"error", "buildingId is required"}}.dump(),
                    "application/json"
                );
                return;
            }

            DbContext db;

            string sql =
                "UPDATE rooms SET energy_strategy = '" + db.escape(strategy) + "' "
                "WHERE floor_id IN ("
                    "SELECT id FROM floors WHERE building_id = " + to_string(buildingId) +
                ")";

            if (mysql_query(db.get(), sql.c_str())) {
                res.status = 500;
                res.set_content(
                    json{{"error", mysql_error(db.get())}}.dump(),
                    "application/json"
                );
                return;
            }

            res.set_content(
                json{
                    {"success", true},
                    {"buildingId", buildingId},
                    {"strategy", strategy},
                    {"affectedRows", (int)mysql_affected_rows(db.get())}
                }.dump(),
                "application/json"
            );
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    // POST SIMULATE STRATEGY
    // ------------------------------------------------------------
    svr.Post("/api/simulate", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = parseJsonBody(req);

            int buildingId = bodyInt(body, "buildingId", 1);
            string strategyKey = normalizeStrategyKey(bodyString(body, "strategy", "normal"));

            DbContext db;

            Building* building = loadBuildingFromDB(db, buildingId);

            if (!building) {
                res.status = 404;
                res.set_content(
                    json{{"error", "Building not found"}}.dump(),
                    "application/json"
                );
                return;
            }

            // Base case: Normal Mode
            building->setStrategy(&normalModeStrategy);

            double baseDaily = building->getDailyKwh();
            double baseWeekly = building->getWeeklyKwh();
            double baseMonthly = building->getMonthlyKwh();

            json baseHourly = json::array();

            for (int h = 0; h < 24; h++) {
                baseHourly.push_back(building->getKwhAtHour(h));
            }

            // Optimized case
            building->setStrategy(keyToStrategy(strategyKey));

            double optDaily = building->getDailyKwh();
            double optWeekly = building->getWeeklyKwh();
            double optMonthly = building->getMonthlyKwh();

            json optHourly = json::array();

            for (int h = 0; h < 24; h++) {
                optHourly.push_back(building->getKwhAtHour(h));
            }

            double savedDaily = baseDaily - optDaily;
            double savedWeekly = baseWeekly - optWeekly;
            double savedMonthly = baseMonthly - optMonthly;

            if (savedDaily < 0) savedDaily = 0;
            if (savedWeekly < 0) savedWeekly = 0;
            if (savedMonthly < 0) savedMonthly = 0;

            json result;
            result["building_id"] = buildingId;
            result["building_name"] = building->getName();
            result["strategy"] = strategyKey;

            result["base_daily_kwh"] = baseDaily;
            result["base_weekly_kwh"] = baseWeekly;
            result["base_monthly_kwh"] = baseMonthly;

            result["opt_daily_kwh"] = optDaily;
            result["opt_weekly_kwh"] = optWeekly;
            result["opt_monthly_kwh"] = optMonthly;

            result["saved_daily_kwh"] = savedDaily;
            result["saved_weekly_kwh"] = savedWeekly;
            result["saved_monthly_kwh"] = savedMonthly;

            result["saved_cost"] = savedMonthly * 3000.0;

            result["base_hourly"] = baseHourly;
            result["opt_hourly"] = optHourly;

            delete building;

            res.set_content(result.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    // ------------------------------------------------------------
    

    // ------------------------------------------------------------
    

    cout << "\n========================================\n";
    cout << "   HTTP API SERVER STARTED\n";
    cout << "========================================\n";
    cout << "Server running at: http://localhost:8080\n";
    cout << "Open index.html in browser or navigate to http://localhost:8080\n";
    cout << "Press Ctrl+C to stop\n";
    cout << "========================================\n";

    svr.listen("0.0.0.0", 8080);
}

// =====================================================
// MAIN MENU
// =====================================================
int main() {
    try {
        DbContext db;

        int choice;

        while (true) {
            cout << "\n========================================\n";
            cout << "   HE THONG QUAN LY NANG LUONG (EMS)\n";
            cout << "========================================\n";
            cout << "1. Bao cao tieu thu dien\n";
            cout << "2. Them Toa nha moi\n";
            cout << "3. Them Tang moi\n";
            cout << "4. Them Phong moi\n";
            cout << "5. Them Thiet bi moi\n";
            cout << "6. Cap nhat trang thai phong theo gio\n";
            cout << "7. Chon che do toi uu nang luong cho Phong\n";
            cout << "10. Khoi dong HTTP API Server (Web Dashboard)\n";
            cout << "0. Thoat\n";
            cout << "----------------------------------------\n";

            choice = getIntInput("Moi ban chon: ");

            if (choice == 0) break;

            if (choice == 1) {
                cout << "\n--- CHON LOAI BAO CAO ---\n";
                cout << "1. Bao cao ngay (chi tiet theo gio)\n";
                cout << "2. Bao cao tuan\n";
                cout << "3. Bao cao thang\n";
                cout << "0. Quay lai\n";

                int reportType = getIntInput("Moi chon: ");

                if (reportType == 0) continue;

                if (reportType < 1 || reportType > 3) {
                    cout << ">> Lua chon khong hop le.\n";
                    continue;
                }

                int b_id = getIntInput("Nhap ID Toa nha: ");

                Building* building = loadBuildingFromDB(db, b_id);

                if (!building) {
                    cout << ">> Khong tim thay Toa nha.\n";
                } else {
                    int mode = reportType - 1;

                    cout << "\n=== ENERGY HIERARCHY ===\n";
                    building->printTree(0, mode);

                    if (mode == 0) {
                        cout << "\n=== CHI TIET THEO GIO (BAO CAO NGAY) ===\n";
                        building->printDetailDaily();
                    }

                    double total;
                    string label;

                    if (mode == 0) {
                        total = building->getDailyKwh();
                        label = "Daily";
                    } else if (mode == 1) {
                        total = building->getWeeklyKwh();
                        label = "Weekly";
                    } else {
                        total = building->getMonthlyKwh();
                        label = "Monthly";
                    }

                    cout << fixed << setprecision(2);
                    cout << "\nTotal " << label << " kWh: " << total << "\n";

                    delete building;
                }
            }

            else if (choice == 2) {
                string code = getStringInput("Ma toa nha: ");
                string name = getStringInput("Ten toa nha: ");

                int new_id = insertBuilding(db, code, name);

                if (new_id > 0) {
                    cout << ">> Tao thanh cong! ID: " << new_id << "\n";
                } else {
                    cout << ">> Tao that bai.\n";
                }
            }

            else if (choice == 3) {
                int b_id = getIntInput("Nhap ID Toa nha chua tang: ");
                string code = getStringInput("Ma tang: ");
                string name = getStringInput("Ten tang: ");

                int new_id = insertFloor(db, b_id, code, name);

                if (new_id > 0) {
                    cout << ">> Tao thanh cong! ID: " << new_id << "\n";
                } else {
                    cout << ">> Tao that bai.\n";
                }
            }

            else if (choice == 4) {
                listFloors(db);

                int f_id = getIntInput(">> Nhap ID Tang: ");
                string code = getStringInput("Ma phong: ");
                string name = getStringInput("Ten phong: ");
                int capacity = getIntInput("Suc chua toi da cua phong: ");

                if (capacity <= 0) capacity = 1;

                int new_id = insertRoom(db, f_id, code, name, capacity);

                if (new_id > 0) {
                    cout << ">> Tao thanh cong! ID: " << new_id << "\n";
                } else {
                    cout << ">> Tao that bai.\n";
                }
            }

            else if (choice == 5) {
                listRooms(db);

                int r_id = getIntInput(">> Nhap ID Phong: ");
                string dev_name = getStringInput("Ten thiet bi: ");

                json attrs;

                // Chon loai thiet bi tu danh sach co san
                cout << "\n--- CHON LOAI THIET BI ---\n";
                cout << "1. Light (Den)\n";
                cout << "2. AC (Dieu hoa)\n";
                cout << "3. Fan (Quat)\n";
                cout << "4. Computer (May tinh)\n";
                cout << "5. Projector (May chieu)\n";
                cout << "6. Refrigerator (Tu lanh)\n";
                cout << "7. TV (Tivi)\n";
                cout << "8. Other (Khac)\n";
                int typeChoice = getIntInput("Chon loai thiet bi: ");
                
                string device_type;
                double defaultPower = 0;
                switch(typeChoice) {
                    case 1: device_type = "light"; defaultPower = 40; break;
                    case 2: device_type = "ac"; defaultPower = 1200; break;
                    case 3: device_type = "fan"; defaultPower = 75; break;
                    case 4: device_type = "computer"; defaultPower = 300; break;
                    case 5: device_type = "projector"; defaultPower = 250; break;
                    case 6: device_type = "refrigerator"; defaultPower = 150; break;
                    case 7: device_type = "tv"; defaultPower = 100; break;
                    case 8: 
                        device_type = getStringInput("Nhap ten loai thiet bi: "); 
                        defaultPower = 100;
                        break;
                    default: 
                        device_type = "other"; 
                        defaultPower = 100;
                        break;
                }
                
                attrs["device_type"] = device_type;
                
                // Chon cong suat
                cout << "\n--- CONG SUAT THIET BI ---\n";
                cout << "Cong suat mac dinh goi y: " << defaultPower << " W\n";
                cout << "1. Su dung cong suat mac dinh\n";
                cout << "2. Nhap cong suat tuy chinh (W)\n";
                int powerChoice = getIntInput("Chon: ");
                if (powerChoice == 2) {
                    attrs["rated_power_w"] = getDoubleInput("Nhap cong suat thiet bi (W): ");
                } else {
                    attrs["rated_power_w"] = defaultPower;
                }
                
                attrs["usage_days_per_week"] = getDoubleInput("So ngay hoat dong moi tuan (0-7): ");

                int tempSensitivity = clampInt(getIntInput("Muc nhay cam voi nhiet do (0-5): "), 0, 5);
                int occupancySensitivity = clampInt(getIntInput("Muc nhay cam voi so nguoi (0-5): "), 0, 5);
                int humiditySensitivity = clampInt(getIntInput("Muc nhay cam voi do am (0-5): "), 0, 5);

                attrs["temp_sensitivity"] = tempSensitivity;
                attrs["occupancy_sensitivity"] = occupancySensitivity;
                attrs["humidity_sensitivity"] = humiditySensitivity;

                // For simple Calculation / Decorator module
                attrs["occupancy_sensitive"] = (occupancySensitivity > 0);
                attrs["humidity_sensitive"] = (humiditySensitivity > 0);

                if (containsAny(device_type, {"light", "den", "đèn"})) {
                    attrs["daylight_sensitive"] = true;
                    attrs["daylight_threshold"] = 500.0;
                }

                json schedule = json::array();
                cout << "\n--- CAU HINH LICH HOAT DONG ---\n";
                cout << "Ban co the them nhieu khoang thoi gian BAT khac nhau (cho phep ngat gio)\n";

                while (true) {
                    cout << "\n--- Them khoang thoi gian thiet bi BAT ---\n";

                    int startHour = getIntInput("Gio bat dau (0-23): ");
                    int endHour = getIntInput("Gio ket thuc (0-23): ");

                    startHour = clampInt(startHour, 0, 23);
                    endHour = clampInt(endHour, 0, 23);

                    json slot;
                    slot["start"] = startHour;
                    slot["end"] = endHour;

                    schedule.push_back(slot);

                    cout << ">> Da them khoang BAT: " << startHour << "h -> " << endHour << "h\n";

                    string more = getStringInput("Them khoang BAT khac (co the tao khoang tat giua)? (y/n): ");

                    if (more != "y" && more != "Y") break;
                }

                attrs["schedule"] = schedule;

                int new_id = insertDevice(db, r_id, dev_name, attrs);

                if (new_id > 0) {
                    cout << ">> Da them thiet bi thanh cong! ID: " << new_id << "\n";
                } else {
                    cout << ">> Them thiet bi that bai.\n";
                }
            }

            else if (choice == 6) {
                listRooms(db);

                int r_id = getIntInput(">> Nhap ID Phong can cap nhat trang thai: ");

                int startHour = getIntInput("Gio bat dau (0-23): ");
                int endHour = getIntInput("Gio ket thuc (0-23): ");

                startHour = clampInt(startHour, 0, 23);
                endHour = clampInt(endHour, 0, 23);

                if (endHour < startHour) swap(startHour, endHour);

                int people = getIntInput("So nguoi trong phong: ");

                if (people < 0) people = 0;

                double temperature = getDoubleInput("Nhiet do moi truong (do C): ");
                double humidity = getDoubleInput("Do am (%): ");
                double lux = getDoubleInput("Anh sang tu nhien (Lux): ");

                int updated = upsertRoomStates(
                    db,
                    r_id,
                    startHour,
                    endHour,
                    people,
                    temperature,
                    humidity,
                    lux
                );

                cout << ">> Da luu trang thai cho " << updated
                     << " gio (tu " << startHour << "h den " << endHour << "h).\n";
            }

            else if (choice == 7) {
                listRooms(db);

                int r_id = getIntInput(">> Nhap ID Phong can doi che do: ");

                cout << "\n--- CHON CHE DO TOI UU NANG LUONG ---\n";
                cout << "1. Normal Mode\n";
                cout << "2. Eco Saving\n";
                cout << "3. Peak Shaving\n";
                cout << "4. Auto Adaptive\n";

                int sChoice = getIntInput("Moi chon: ");

                string stratKey;

                switch (sChoice) {
                    case 1:
                        stratKey = "normal";
                        break;
                    case 2:
                        stratKey = "eco";
                        break;
                    case 3:
                        stratKey = "peak";
                        break;
                    case 4:
                        stratKey = "auto";
                        break;
                    default:
                        cout << ">> Lua chon khong hop le.\n";
                        continue;
                }

                if (updateRoomStrategy(db, r_id, stratKey)) {
                    cout << ">> Da cap nhat che do nang luong cho phong thanh cong!\n";
                } else {
                    cout << ">> Cap nhat that bai.\n";
                }
            }

            else if (choice == 10) {
                startHttpServer();
            }
        }
    } catch (const exception& e) {
        cerr << "Loi: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
