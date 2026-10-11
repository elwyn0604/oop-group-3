#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <limits>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <mysql.h>

using namespace std;
using json = nlohmann::json;

// =====================================================
// THÔNG TIN KẾT NỐI MYSQL
// =====================================================
static const string DB_HOST = "127.0.0.1";
static const string DB_USER = "root";
static const string DB_PASSWORD = "Minhdat1";
static const string DB_SCHEMA = "energy_management_db";

// =====================================================
// HÀM HỖ TRỢ NHẬP LIỆU
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
        cout << ">> Dữ liệu không hợp lệ. Vui lòng nhập một số nguyên.\n";
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
        cout << ">> Dữ liệu không hợp lệ. Vui lòng nhập số.\n";
    }
}

int clampInt(int value, int minValue, int maxValue) {
    if (value < minValue) return minValue;
    if (value > maxValue) return maxValue;
    return value;
}

// =====================================================
// HÀM HỖ TRỢ ĐỌC JSON
// =====================================================
double getJsonNumber(const json& attrs, const string& key, double defaultValue) {
    if (!attrs.contains(key)) return defaultValue;
    if (attrs[key].is_number()) return attrs[key].get<double>();
    if (attrs[key].is_string()) {
        try { return stod(attrs[key].get<string>()); } catch (...) { return defaultValue; }
    }
    return defaultValue;
}

string getJsonString(const json& attrs, const string& key, const string& defaultValue) {
    if (!attrs.contains(key)) return defaultValue;
    if (attrs[key].is_string()) return attrs[key].get<string>();
    return defaultValue;
}

int safeGetInt(const string& s) {
    if (s.empty()) return 0;
    try { return stoi(s); } catch (...) { return 0; }
}

double safeGetDouble(const string& s, double defaultValue = 0.0) {
    if (s.empty()) return defaultValue;
    try { return stod(s); } catch (...) { return defaultValue; }
}

// =====================================================
// =====================================================
// BLOCK: ROOM MODEL
// Trạng thái môi trường của phòng theo từng giờ (0-23)
// =====================================================
// =====================================================
class RoomState {
public:
    int peopleCount;
    double temperatureC;
    bool hasData;

    RoomState() : peopleCount(0), temperatureC(25.0), hasData(false) {}
};

class RoomDayProfile {
public:
    int capacity;
    RoomState hours[24];

    RoomDayProfile() : capacity(10) {}

    // Lấy trạng thái tại một giờ. Nếu giờ đó chưa có dữ liệu,
    // lùi dần về các giờ trước đó để tìm giá trị gần nhất.
    RoomState getStateAt(int hour) const {
        hour = ((hour % 24) + 24) % 24;

        if (hours[hour].hasData) return hours[hour];

        for (int step = 1; step < 24; step++) {
            int h = (hour - step + 24) % 24;
            if (hours[h].hasData) return hours[h];
        }

        return RoomState(); // Mặc định: 0 người, 25 độ
    }
};

// =====================================================
// =====================================================
// BLOCK: STRATEGY
// =====================================================
// =====================================================
class IEnergyStrategy {
public:
    virtual ~IEnergyStrategy() {}
    virtual string getName() = 0;
    // Trả về hệ số điều chỉnh (factor).
    // 1.0 = bình thường, >1 = tốn điện hơn, <1 = tiết kiệm hơn.
    virtual double calculateFactor(const json& attrs, const RoomState& state, int capacity) = 0;
};

double occupancyRateOf(const RoomState& state, int capacity) {
    if (capacity <= 0) return 1.0;
    double rate = (double)state.peopleCount / capacity;
    if (rate > 1.0) rate = 1.0;
    if (rate < 0.0) rate = 0.0;
    return rate;
}

// Strategy 1: Không điều chỉnh gì
class StandardStrategy : public IEnergyStrategy {
public:
    string getName() override { return "Standard"; }
    double calculateFactor(const json& attrs, const RoomState& state, int capacity) override {
        return 1.0;
    }
};

// Strategy 2: Giảm điện khi phòng vắng người
class EcoStrategy : public IEnergyStrategy {
public:
    string getName() override { return "Eco Saving"; }
    double calculateFactor(const json& attrs, const RoomState& state, int capacity) override {
        int os = clampInt((int)getJsonNumber(attrs, "occupancy_sensitivity", 0), 0, 5);
        if (os <= 0) return 1.0;

        double occ = occupancyRateOf(state, capacity);
        double reduction = 0.0;
        if (occ <= 0.0) reduction = os * 0.12;
        else if (occ < 0.3) reduction = os * 0.08;
        else if (occ < 0.6) reduction = os * 0.04;

        double factor = 1.0 - reduction;
        if (factor < 0.2) factor = 0.2;
        return factor;
    }
};

// Strategy 3: Tăng điện khi trời nóng
class PeakStrategy : public IEnergyStrategy {
public:
    string getName() override { return "Peak / Hot Environment"; }
    double calculateFactor(const json& attrs, const RoomState& state, int capacity) override {
        int ts = clampInt((int)getJsonNumber(attrs, "temp_sensitivity", 0), 0, 5);
        if (ts <= 0) return 1.0;

        double temp = state.temperatureC;
        double addition = 0.0;
        if (temp >= 35.0) addition = ts * 0.07;
        else if (temp >= 30.0) addition = ts * 0.05;
        else if (temp <= 22.0) addition = -ts * 0.02;

        double factor = 1.0 + addition;
        if (factor < 0.5) factor = 0.5;
        return factor;
    }
};

// Strategy 4: Kết hợp cả người và nhiệt độ
class AutoStrategy : public IEnergyStrategy {
public:
    string getName() override { return "Auto Adaptive"; }
    double calculateFactor(const json& attrs, const RoomState& state, int capacity) override {
        int ts = clampInt((int)getJsonNumber(attrs, "temp_sensitivity", 0), 0, 5);
        int os = clampInt((int)getJsonNumber(attrs, "occupancy_sensitivity", 0), 0, 5);

        double factor = 1.0;

        if (os > 0) {
            double occ = occupancyRateOf(state, capacity);
            double reduction = 0.0;
            if (occ <= 0.0) reduction = os * 0.12;
            else if (occ < 0.3) reduction = os * 0.08;
            else if (occ < 0.6) reduction = os * 0.04;
            factor *= (1.0 - reduction);
        }

        if (ts > 0) {
            double temp = state.temperatureC;
            double addition = 0.0;
            if (temp >= 35.0) addition = ts * 0.07;
            else if (temp >= 30.0) addition = ts * 0.05;
            else if (temp <= 22.0) addition = -ts * 0.02;
            factor *= (1.0 + addition);
        }

        if (factor < 0.1) factor = 0.1;
        return factor;
    }
};

// Các object strategy dùng chung
StandardStrategy standardStrategy;
EcoStrategy ecoStrategy;
PeakStrategy peakStrategy;
AutoStrategy autoStrategy;

// Hệ thống tự chọn strategy dựa trên sensitivity của thiết bị
IEnergyStrategy* pickStrategy(const json& attrs) {
    int ts = clampInt((int)getJsonNumber(attrs, "temp_sensitivity", 0), 0, 5);
    int os = clampInt((int)getJsonNumber(attrs, "occupancy_sensitivity", 0), 0, 5);

    if (ts > 0 && os > 0) return &autoStrategy;
    if (os > 0) return &ecoStrategy;
    if (ts > 0) return &peakStrategy;
    return &standardStrategy;
}

// =====================================================
// =====================================================
// BLOCK: COMPOSITE
// =====================================================
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
    virtual void addChild(IComponent* child) {}
    virtual void printTree(int level, int mode) = 0;
    virtual void printDetailDaily() {}
    virtual void setDayProfile(RoomDayProfile* profile) {}
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

    int getId() override { return id; }
    string getName() override { return name; }

    void addChild(IComponent* child) override {
        if (child) children.push_back(child);
    }

    double getDailyKwh() override {
        double total = 0.0;
        for (int i = 0; i < (int)children.size(); i++) total += children[i]->getDailyKwh();
        return total;
    }

    double getWeeklyKwh() override {
        double total = 0.0;
        for (int i = 0; i < (int)children.size(); i++) total += children[i]->getWeeklyKwh();
        return total;
    }

    double getMonthlyKwh() override {
        double total = 0.0;
        for (int i = 0; i < (int)children.size(); i++) total += children[i]->getMonthlyKwh();
        return total;
    }

    // mode: 0 = ngày, 1 = tuần, 2 = tháng
    void printTree(int level, int mode) override {
        string indent(level * 2, ' ');
        double value;
        string label;
        if (mode == 0) { value = getDailyKwh(); label = "Daily"; }
        else if (mode == 1) { value = getWeeklyKwh(); label = "Weekly"; }
        else { value = getMonthlyKwh(); label = "Monthly"; }

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
};

class Building : public CompositeComponent {
public:
    Building(int id, string code, string name) : CompositeComponent(id, name) {}
    string getKind() override { return "Building"; }
};

class Floor : public CompositeComponent {
public:
    Floor(int id, string code, string name) : CompositeComponent(id, name) {}
    string getKind() override { return "Floor"; }
};

class Room : public CompositeComponent {
private:
    int capacity;
    RoomDayProfile* profile; // Room sở hữu profile này

public:
    Room(int id, string code, string name, int cap)
        : CompositeComponent(id, name), capacity(cap) {
        profile = new RoomDayProfile();
        profile->capacity = cap;
    }

    ~Room() {
        delete profile;
    }

    string getKind() override { return "Room"; }
    int getCapacity() { return capacity; }
    RoomDayProfile* getProfile() { return profile; }

    void printTree(int level, int mode) override {
        string indent(level * 2, ' ');
        double value;
        string label;
        if (mode == 0) { value = getDailyKwh(); label = "Daily"; }
        else if (mode == 1) { value = getWeeklyKwh(); label = "Weekly"; }
        else { value = getMonthlyKwh(); label = "Monthly"; }

        cout << fixed << setprecision(2);
        cout << indent << "[Room] " << name
             << " (sức chứa: " << capacity << ")"
             << " - " << label << ": " << value << " kWh\n";

        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->printTree(level + 1, mode);
        }
    }

    // Sau khi load trạng thái phòng xong, truyền profile cho các thiết bị
    void applyProfileToChildren() {
        for (int i = 0; i < (int)children.size(); i++) {
            children[i]->setDayProfile(profile);
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
            if (!attributes.is_object()) attributes = json::object();
        } else {
            attributes = json::object();
        }

        ratedPower = getJsonNumber(attributes, "rated_power_w", 0.0);
        usageDaysPerWeek = getJsonNumber(attributes, "usage_days_per_week", 7.0);
        if (usageDaysPerWeek > 7) usageDaysPerWeek = 7;
        if (usageDaysPerWeek < 0) usageDaysPerWeek = 0;

        strategy = pickStrategy(attributes);

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

    int getId() override { return id; }
    string getName() override { return name; }
    string getKind() override { return "Device"; }

    void setDayProfile(RoomDayProfile* p) override {
        profile = p;
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
                // Khoảng vắt qua nửa đêm, ví dụ 22 -> 2
                if (hour >= s || hour < e) return true;
            }
        }
        return false;
    }

    double getDailyKwh() override {
        double total = 0.0;
        for (int hour = 0; hour < 24; hour++) {
            if (!isOnAt(hour)) continue;
            RoomState state = profile ? profile->getStateAt(hour) : RoomState();
            int cap = profile ? profile->capacity : 10;
            double factor = strategy->calculateFactor(attributes, state, cap);
            total += (ratedPower / 1000.0) * 1.0 * factor;
        }
        return total;
    }

    double getWeeklyKwh() override {
        return getDailyKwh() * usageDaysPerWeek;
    }

    double getMonthlyKwh() override {
        return getWeeklyKwh() * 52.0 / 12.0;
    }

    void printTree(int level, int mode) override {
        string indent(level * 2, ' ');
        double value;
        string label;
        if (mode == 0) { value = getDailyKwh(); label = "Daily"; }
        else if (mode == 1) { value = getWeeklyKwh(); label = "Weekly"; }
        else { value = getMonthlyKwh(); label = "Monthly"; }

        cout << fixed << setprecision(2);
        cout << indent << "[Device] " << name
             << " (" << strategy->getName() << ")"
             << " - " << label << ": " << value << " kWh\n";
    }

    void printDetailDaily() override {
        cout << fixed << setprecision(2);
        cout << "\n  [Chi tiết theo giờ] " << name
             << " - Strategy: " << strategy->getName() << "\n";

        double dayTotal = 0.0;
        bool hasOnHour = false;

        for (int hour = 0; hour < 24; hour++) {
            if (!isOnAt(hour)) continue;
            hasOnHour = true;

            RoomState state = profile ? profile->getStateAt(hour) : RoomState();
            int cap = profile ? profile->capacity : 10;
            double factor = strategy->calculateFactor(attributes, state, cap);
            double kwh = (ratedPower / 1000.0) * 1.0 * factor;
            dayTotal += kwh;

            cout << "    " << setw(2) << hour << "h: BẬT "
                 << " | " << state.peopleCount << " người"
                 << " | " << state.temperatureC << " độ C"
                 << " | hệ số " << factor
                 << " | " << kwh << " kWh\n";
        }

        if (!hasOnHour) {
            cout << "    (Thiết bị không bật giờ nào trong ngày)\n";
        } else {
            cout << "    Tổng ngày: " << dayTotal << " kWh\n";
        }
    }
};

// =====================================================
// XỬ LÝ MYSQL
// =====================================================
class DbContext {
private:
    MYSQL* conn;

public:
    DbContext() {
        conn = mysql_init(nullptr);
        if (!mysql_real_connect(conn, DB_HOST.c_str(), DB_USER.c_str(), DB_PASSWORD.c_str(), DB_SCHEMA.c_str(), 3306, nullptr, 0)) {
            throw runtime_error("Lỗi kết nối MySQL: " + string(mysql_error(conn)));
        }
        mysql_set_character_set(conn, "utf8mb4");
    }

    ~DbContext() { if (conn) mysql_close(conn); }

    MYSQL* get() { return conn; }

    string escape(const string& s) {
        string out(s.size() * 2 + 1, '\0');
        unsigned long len = mysql_real_escape_string(conn, &out[0], s.c_str(), s.length());
        out.resize(len);
        return out;
    }
};

vector<vector<string>> executeQuery(MYSQL* conn, const string& sql) {
    if (mysql_query(conn, sql.c_str())) {
        throw runtime_error("Lỗi SQL: " + string(mysql_error(conn)));
    }
    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) return {};

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
// LOAD DỮ LIỆU TỪ DB
// =====================================================
Building* loadBuildingFromDB(DbContext& db, int buildingId) {
    string sqlB = "SELECT id, building_code, name FROM buildings WHERE id = " + to_string(buildingId);
    auto b_rows = executeQuery(db.get(), sqlB);
    if (b_rows.empty()) return nullptr;

    Building* building = new Building(safeGetInt(b_rows[0][0]), b_rows[0][1], b_rows[0][2]);

    string sqlF = "SELECT id, floor_code, name FROM floors WHERE building_id = " + to_string(buildingId);
    auto f_rows = executeQuery(db.get(), sqlF);

    for (const auto& f : f_rows) {
        int floorId = safeGetInt(f[0]);
        Floor* floor = new Floor(floorId, f[1], f[2]);

        string sqlR = "SELECT id, room_code, name, capacity FROM rooms WHERE floor_id = " + to_string(floorId);
        auto r_rows = executeQuery(db.get(), sqlR);

        for (const auto& r : r_rows) {
            int roomId = safeGetInt(r[0]);
            string roomCode = r[1];
            string roomName = r[2];
            int capacity = safeGetInt(r[3]);
            if (capacity <= 0) capacity = 10;

            Room* room = new Room(roomId, roomCode, roomName, capacity);

            // Load trạng thái phòng theo giờ
            string sqlS = "SELECT hour_of_day, people_count, temperature_c FROM room_states WHERE room_id = " + to_string(roomId);
            auto s_rows = executeQuery(db.get(), sqlS);
            RoomDayProfile* profile = room->getProfile();
            for (const auto& srow : s_rows) {
                int h = safeGetInt(srow[0]);
                if (h < 0 || h > 23) continue;
                profile->hours[h].peopleCount = safeGetInt(srow[1]);
                profile->hours[h].temperatureC = safeGetDouble(srow[2], 25.0);
                profile->hours[h].hasData = true;
            }

            // Load thiết bị
            string sqlD = "SELECT id, device_name, attributes FROM devices WHERE room_id = " + to_string(roomId);
            auto d_rows = executeQuery(db.get(), sqlD);
            for (const auto& d : d_rows) {
                Device* device = new Device(safeGetInt(d[0]), d[1], d[2]);
                room->addChild(device);
            }

            // Truyền trạng thái phòng cho thiết bị
            room->applyProfileToChildren();

            floor->addChild(room);
        }

        building->addChild(floor);
    }

    return building;
}

// =====================================================
// CÁC HÀM INSERT
// =====================================================
int insertBuilding(DbContext& db, const string& code, const string& name) {
    string sql = "INSERT INTO buildings (building_code, name) VALUES ('" + db.escape(code) + "', '" + db.escape(name) + "')";
    if (mysql_query(db.get(), sql.c_str())) return -1;
    return (int)mysql_insert_id(db.get());
}

int insertFloor(DbContext& db, int b_id, const string& code, const string& name) {
    string sql = "INSERT INTO floors (building_id, floor_code, name) VALUES (" + to_string(b_id) + ", '" + db.escape(code) + "', '" + db.escape(name) + "')";
    if (mysql_query(db.get(), sql.c_str())) return -1;
    return (int)mysql_insert_id(db.get());
}

int insertRoom(DbContext& db, int f_id, const string& code, const string& name, int capacity) {
    string sql = "INSERT INTO rooms (floor_id, room_code, name, capacity) VALUES ("
        + to_string(f_id) + ", '" + db.escape(code) + "', '" + db.escape(name) + "', " + to_string(capacity) + ")";
    if (mysql_query(db.get(), sql.c_str())) return -1;
    return (int)mysql_insert_id(db.get());
}

void insertDevice(DbContext& db, int r_id, const string& name, const json& attrs) {
    string sql = "INSERT INTO devices (room_id, device_name, attributes) VALUES ("
        + to_string(r_id) + ", '" + db.escape(name) + "', '" + db.escape(attrs.dump()) + "')";
    if (mysql_query(db.get(), sql.c_str())) {
        cout << "Lỗi thêm Device: " << mysql_error(db.get()) << "\n";
    } else {
        cout << ">> Đã thêm thiết bị thành công!\n";
    }
}

void upsertRoomStates(DbContext& db, int roomId, int startHour, int endHour, int people, double temperature) {
    int successCount = 0;
    for (int h = startHour; h <= endHour; h++) {
        if (h < 0 || h > 23) continue;
        string sql = "INSERT INTO room_states (room_id, hour_of_day, people_count, temperature_c) VALUES ("
            + to_string(roomId) + ", " + to_string(h) + ", " + to_string(people) + ", " + to_string(temperature)
            + ") ON DUPLICATE KEY UPDATE people_count = " + to_string(people) + ", temperature_c = " + to_string(temperature);
        if (mysql_query(db.get(), sql.c_str()) == 0) successCount++;
    }
    cout << ">> Đã lưu trạng thái cho " << successCount << " giờ (từ " << startHour << "h đến " << endHour << "h).\n";
}

// =====================================================
// LIST FLOORS / ROOMS
// =====================================================
void listFloors(DbContext& db) {
    auto rows = executeQuery(db.get(), "SELECT f.id, f.floor_code, f.name, b.name FROM floors f JOIN buildings b ON f.building_id = b.id ORDER BY f.id");
    if (rows.empty()) { cout << ">> Chưa có Tầng nào.\n"; return; }
    cout << "\n--- DANH SÁCH CÁC TẦNG ---\n";
    cout << "ID | Mã tầng | Tên tầng | Thuộc tòa nhà\n";
    for (const auto& r : rows) {
        cout << r[0] << " | " << r[1] << " | " << r[2] << " | " << r[3] << "\n";
    }
}

void listRooms(DbContext& db) {
    auto rows = executeQuery(db.get(), "SELECT r.id, r.room_code, r.name, f.name, r.capacity FROM rooms r JOIN floors f ON r.floor_id = f.id ORDER BY r.id");
    if (rows.empty()) { cout << ">> Chưa có Phòng nào.\n"; return; }
    cout << "\n--- DANH SÁCH CÁC PHÒNG ---\n";
    cout << "ID | Mã phòng | Tên phòng | Thuộc tầng | Sức chứa\n";
    for (const auto& r : rows) {
        cout << r[0] << " | " << r[1] << " | " << r[2] << " | " << r[3] << " | " << r[4] << "\n";
    }
}

// =====================================================
// MAIN - MENU
// =====================================================
int main() {
    try {
        DbContext db;
        int choice;

        while (true) {
            cout << "\n========================================\n";
            cout << "   HỆ THỐNG QUẢN LÝ NĂNG LƯỢNG (EMS)\n";
            cout << "========================================\n";
            cout << "1. Báo cáo tiêu thụ điện\n";
            cout << "2. Thêm Tòa nhà mới\n";
            cout << "3. Thêm Tầng mới\n";
            cout << "4. Thêm Phòng mới\n";
            cout << "5. Thêm Thiết bị mới\n";
            cout << "6. Cập nhật trạng thái phòng theo giờ\n";
            cout << "0. Thoát\n";
            cout << "----------------------------------------\n";

            choice = getIntInput("Mời bạn chọn: ");

            if (choice == 0) break;

            if (choice == 1) {
                cout << "\n--- CHỌN LOẠI BÁO CÁO ---\n";
                cout << "1. Báo cáo ngày (chi tiết theo giờ)\n";
                cout << "2. Báo cáo tuần\n";
                cout << "3. Báo cáo tháng\n";
                cout << "0. Quay lại\n";
                int reportType = getIntInput("Mời chọn: ");
                if (reportType == 0) continue;
                if (reportType < 1 || reportType > 3) {
                    cout << ">> Lựa chọn không hợp lệ.\n";
                    continue;
                }

                int b_id = getIntInput("Nhập ID Tòa nhà: ");
                Building* building = loadBuildingFromDB(db, b_id);
                if (!building) {
                    cout << ">> Không tìm thấy Tòa nhà.\n";
                } else {
                    int mode = reportType - 1; // 0 ngày, 1 tuần, 2 tháng
                    cout << "\n=== ENERGY HIERARCHY ===\n";
                    building->printTree(0, mode);

                    if (mode == 0) {
                        cout << "\n=== CHI TIẾT THEO GIỜ (BÁO CÁO NGÀY) ===\n";
                        building->printDetailDaily();
                    }

                    double total;
                    string label;
                    if (mode == 0) { total = building->getDailyKwh(); label = "Daily"; }
                    else if (mode == 1) { total = building->getWeeklyKwh(); label = "Weekly"; }
                    else { total = building->getMonthlyKwh(); label = "Monthly"; }

                    cout << fixed << setprecision(2);
                    cout << "\nTotal " << label << " kWh: " << total << "\n";

                    delete building;
                }
            }
            else if (choice == 2) {
                string code = getStringInput("Mã tòa nhà: ");
                string name = getStringInput("Tên tòa nhà: ");
                int new_id = insertBuilding(db, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
                else cout << ">> Tạo thất bại.\n";
            }
            else if (choice == 3) {
                int b_id = getIntInput("Nhập ID Tòa nhà chứa tầng: ");
                string code = getStringInput("Mã tầng: ");
                string name = getStringInput("Tên tầng: ");
                int new_id = insertFloor(db, b_id, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
                else cout << ">> Tạo thất bại.\n";
            }
            else if (choice == 4) {
                listFloors(db);
                int f_id = getIntInput(">> Nhập ID Tầng: ");
                string code = getStringInput("Mã phòng: ");
                string name = getStringInput("Tên phòng: ");
                int capacity = getIntInput("Sức chứa tối đa của phòng: ");
                if (capacity <= 0) capacity = 1;
                int new_id = insertRoom(db, f_id, code, name, capacity);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
                else cout << ">> Tạo thất bại.\n";
            }
            else if (choice == 5) {
                listRooms(db);
                int r_id = getIntInput(">> Nhập ID Phòng: ");
                string dev_name = getStringInput("Tên thiết bị: ");

                json attrs;
                attrs["device_type"] = getStringInput("Loại thiết bị (chỉ để tham khảo): ");
                attrs["rated_power_w"] = getDoubleInput("Công suất (W): ");
                attrs["usage_days_per_week"] = getDoubleInput("Số ngày hoạt động mỗi tuần (0-7): ");
                attrs["temp_sensitivity"] = clampInt(getIntInput("Mức nhạy cảm với nhiệt độ (0-5): "), 0, 5);
                attrs["occupancy_sensitivity"] = clampInt(getIntInput("Mức nhạy cảm với số người (0-5): "), 0, 5);

                json schedule = json::array();
                while (true) {
                    cout << "\n--- Thêm khoảng thời gian thiết bị BẬT ---\n";
                    int startHour = getIntInput("Giờ bắt đầu (0-23): ");
                    int endHour = getIntInput("Giờ kết thúc (0-23, tại giờ này thiết bị đã tắt): ");
                    startHour = clampInt(startHour, 0, 23);
                    endHour = clampInt(endHour, 0, 23);

                    json slot;
                    slot["start"] = startHour;
                    slot["end"] = endHour;
                    schedule.push_back(slot);

                    cout << ">> Đã thêm khoảng " << startHour << "h -> " << endHour << "h\n";
                    string more = getStringInput("Thêm khoảng bật khác? (y/n): ");
                    if (more != "y" && more != "Y") break;
                }
                attrs["schedule"] = schedule;

                insertDevice(db, r_id, dev_name, attrs);
            }
            else if (choice == 6) {
                listRooms(db);
                int r_id = getIntInput(">> Nhập ID Phòng cần cập nhật trạng thái: ");
                int startHour = getIntInput("Giờ bắt đầu (0-23): ");
                int endHour = getIntInput("Giờ kết thúc (0-23): ");
                startHour = clampInt(startHour, 0, 23);
                endHour = clampInt(endHour, 0, 23);
                if (endHour < startHour) swap(startHour, endHour);

                int people = getIntInput("Số người trong phòng: ");
                if (people < 0) people = 0;
                double temperature = getDoubleInput("Nhiệt độ môi trường (độ C): ");

                upsertRoomStates(db, r_id, startHour, endHour, people, temperature);
            }
        }
    } catch (const exception& e) {
        cerr << "Lỗi: " << e.what() << "\n";
        return 1;
    }

    return 0;
}