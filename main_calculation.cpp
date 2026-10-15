#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <limits>
#include <iomanip>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <mysql.h>

using namespace std;
using json = nlohmann::json;

static const string DB_HOST = "127.0.0.1";
static const string DB_USER = "root";
static const string DB_PASSWORD = "Minhdat1";
static const string DB_SCHEMA = "energy_management_db";

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

// =====================================================
// BLOCK CALCULATION: tính điện năng tại một thời điểm
// =====================================================
struct Environment {
    int occupancy = 0;          // số người trong phòng
    double roomTemperature = 25.0;
    double outdoorTemperature = 30.0;
    double humidity = 60.0;
    double naturalLight = 50.0; // phần trăm ánh sáng tự nhiên (0-100)
};

struct EnergyResult {
    string deviceName;
    double powerW = 0.0;        // công suất thực tế tại thời điểm mô phỏng
    double energyKwh = 0.0;     // điện năng trong khoảng thời gian tính (kWh)
    bool isOn = false;
};

class EnergyCalculator {
public:
    // Điện năng (kWh) = Công suất (W) / 1000 * thời gian (giờ)
    static double calculateKwh(double powerW, double hours) {
        if (powerW < 0.0 || hours < 0.0) return 0.0;
        return powerW / 1000.0 * hours;
    }

    static double getNumber(const json& attrs, const string& key, double defaultValue) {
        if (!attrs.contains(key)) return defaultValue;
        if (attrs[key].is_number()) return attrs[key].get<double>();
        if (attrs[key].is_string()) {
            try { return stod(attrs[key].get<string>()); }
            catch (...) { return defaultValue; }
        }
        return defaultValue;
    }

    static bool getBool(const json& attrs, const string& key, bool defaultValue) {
        if (!attrs.contains(key)) return defaultValue;
        if (attrs[key].is_boolean()) return attrs[key].get<bool>();
        if (attrs[key].is_number()) return attrs[key].get<int>() != 0;
        if (attrs[key].is_string()) {
            string value = attrs[key].get<string>();
            return value == "true" || value == "1" || value == "on" || value == "ON";
        }
        return defaultValue;
    }

    // Kiểm tra thiết bị có hoạt động ở giờ mô phỏng hay không.
    static bool isOperatingAt(const json& attrs, int hour) {
        if (!getBool(attrs, "is_on", true)) return false;
        double usageHours = getNumber(attrs, "usage_hours_per_day", 24.0);
        double startHour = getNumber(attrs, "start_hour", 0.0);
        if (usageHours <= 0.0) return false;
        if (usageHours >= 24.0) return true;

        int start = ((static_cast<int>(startHour) % 24) + 24) % 24;
        int duration = static_cast<int>(usageHours);
        int end = (start + duration) % 24;
        if (start + duration <= 24) return hour >= start && hour < start + duration;
        return hour >= start || hour < end;
    }

    static EnergyResult calculateDevice(
        const string& name, const json& attrs, const Environment& env, int hour
    ) {
        EnergyResult result;
        result.deviceName = name;
        result.isOn = isOperatingAt(attrs, hour);
        if (!result.isOn) return result;

        double ratedPower = getNumber(attrs, "rated_power_w", 0.0);
        double loadFactor = getNumber(attrs, "load_factor", 1.0);
        if (loadFactor < 0.0) loadFactor = 0.0;
        if (loadFactor > 1.0) loadFactor = 1.0;

        // Thiết bị phụ thuộc số người có thể đặt occupancy_sensitive=true trong attributes.
        if (getBool(attrs, "occupancy_sensitive", false) && env.occupancy <= 0) {
            result.isOn = false;
            return result;
        }

        string type = attrs.value("device_type", string(""));
        // Có thể khai báo daylight_sensitive=true cho đèn; đủ sáng tự nhiên thì tắt đèn.
        bool isLight = (type == "Light" || type == "light" || type == "Đèn" ||
                        type == "Den" || type == "den");
        if (isLight && getBool(attrs, "daylight_sensitive", false) &&
            env.naturalLight >= getNumber(attrs, "daylight_threshold", 80.0)) {
            result.isOn = false;
            return result;
        }

        double environmentFactor = 1.0;
        // Mô hình đơn giản: điều hòa tiêu thụ tăng 5% cho mỗi độ trên 25°C.
        bool isAC = (type == "AC" || type == "ac" || type == "Air Conditioner" ||
                     type == "air_conditioner" || type == "Điều hòa");
        if (isAC)
            environmentFactor += max(0.0, env.roomTemperature - 25.0) * 0.05;

        // Có thể khai báo humidity_sensitive=true cho thiết bị chịu ảnh hưởng độ ẩm.
        if (getBool(attrs, "humidity_sensitive", false))
            environmentFactor += max(0.0, env.humidity - 60.0) * 0.005;

        result.powerW = ratedPower * loadFactor * environmentFactor;
        result.energyKwh = calculateKwh(result.powerW, 1.0);
        return result;
    }
};

// =====================================================
// BLOCK COMPOSITE: IComponent + Composite Pattern
// =====================================================
class IComponent {
public:
    virtual ~IComponent() {}
    virtual int getId() = 0;
    virtual string getName() = 0;
    virtual string getKind() = 0;
    virtual double getDailyKwh() = 0;
    virtual double getMonthlyKwh() = 0;
    virtual void addChild(IComponent* child) {}
    virtual void printTree(int level) = 0;
};

class CompositeComponent : public IComponent {
protected:
    int id;
    string name;
    vector<IComponent*> children;
public:
    CompositeComponent(int _id, string _name) : id(_id), name(_name) {}
    ~CompositeComponent() {
        for (int i = 0; i < (int)children.size(); i++) delete children[i];
    }
    int getId() override { return id; }
    string getName() override { return name; }
    void addChild(IComponent* child) override {
        if (child) children.push_back(child);
    }
    const vector<IComponent*>& getChildren() const { return children; }
    double getDailyKwh() override {
        double total = 0.0;
        for (auto c : children) total += c->getDailyKwh();
        return total;
    }
    double getMonthlyKwh() override {
        double total = 0.0;
        for (auto c : children) total += c->getMonthlyKwh();
        return total;
    }
    void printTree(int level) override {
        string indent(level * 2, ' ');
        cout << fixed << setprecision(2);
        cout << indent << "[" << getKind() << "] " << name
             << " - Daily: " << getDailyKwh() << " kWh"
             << " - Monthly: " << getMonthlyKwh() << " kWh\n";
        for (auto c : children) c->printTree(level + 1);
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
public:
    Room(int id, string code, string name) : CompositeComponent(id, name) {}
    string getKind() override { return "Room"; }
};

class Device : public IComponent {
private:
    int id;
    string name;
    json attributes;

    double getNumber(const string& key, double defaultVal) {
        if (!attributes.contains(key)) return defaultVal;
        if (attributes[key].is_number()) return attributes[key].get<double>();
        if (attributes[key].is_string()) {
            try { return stod(attributes[key].get<string>()); } catch (...) { return defaultVal; }
        }
        return defaultVal;
    }

    double weeklyKwh() {
        double power = getNumber("rated_power_w", 0.0);
        double hours = getNumber("usage_hours_per_day", 0.0);
        double days = getNumber("usage_days_per_week", 7.0);
        double load = getNumber("load_factor", 1.0);
        if (hours > 24) hours = 24;
        if (days > 7) days = 7;
        if (load > 1) load = 1;
        return (power / 1000.0) * hours * days * load;
    }

public:
    Device(int _id, string _name, const string& attrJson) : id(_id), name(_name) {
        if (!attrJson.empty()) {
            attributes = json::parse(attrJson, nullptr, false);
            if (!attributes.is_object()) attributes = json::object();
        } else {
            attributes = json::object();
        }
    }

    int getId() override { return id; }
    string getName() override { return name; }
    string getKind() override { return "Device"; }
    const json& getAttributes() const { return attributes; }
    double getDailyKwh() override { return weeklyKwh() / 7.0; }
    double getMonthlyKwh() override { return weeklyKwh() * 52.0 / 12.0; }

    void printTree(int level) override {
        string indent(level * 2, ' ');
        cout << fixed << setprecision(2);
        cout << indent << "[Device] " << name
             << " - Daily: " << getDailyKwh() << " kWh"
             << " - Monthly: " << getMonthlyKwh() << " kWh\n";
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
    if (mysql_query(conn, sql.c_str()))
        throw runtime_error("Lỗi SQL: " + string(mysql_error(conn)));
    MYSQL_RES* result = mysql_store_result(conn);
    if (!result) return {};
    vector<vector<string>> rows;
    int num_fields = mysql_num_fields(result);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        vector<string> row_data;
        for (int i = 0; i < num_fields; i++)
            row_data.push_back(row[i] ? string(row[i]) : "");
        rows.push_back(row_data);
    }
    mysql_free_result(result);
    return rows;
}

int safeGetInt(const string& s) {
    if (s.empty()) return 0;
    try { return stoi(s); } catch (...) { return 0; }
}

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

        string sqlR = "SELECT id, room_code, name FROM rooms WHERE floor_id = " + to_string(floorId);
        auto r_rows = executeQuery(db.get(), sqlR);
        for (const auto& r : r_rows) {
            Room* room = new Room(safeGetInt(r[0]), r[1], r[2]);

            string sqlD = "SELECT id, device_name, attributes FROM devices WHERE room_id = " + to_string(room->getId());
            auto d_rows = executeQuery(db.get(), sqlD);
            for (const auto& d : d_rows)
                room->addChild(new Device(safeGetInt(d[0]), d[1], d[2]));

            floor->addChild(room);
        }
        building->addChild(floor);
    }
    return building;
}

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

int insertRoom(DbContext& db, int f_id, const string& code, const string& name) {
    string sql = "INSERT INTO rooms (floor_id, room_code, name) VALUES (" + to_string(f_id) + ", '" + db.escape(code) + "', '" + db.escape(name) + "')";
    if (mysql_query(db.get(), sql.c_str())) return -1;
    return (int)mysql_insert_id(db.get());
}

void insertDevice(DbContext& db, int r_id, const string& name, const json& attrs) {
    string sql = "INSERT INTO devices (room_id, device_name, attributes) VALUES (" + to_string(r_id) + ", '" + db.escape(name) + "', '" + db.escape(attrs.dump()) + "')";
    if (mysql_query(db.get(), sql.c_str())) cout << "Lỗi thêm Device\n";
    else cout << ">> Đã thêm thiết bị thành công!\n";
}

void listFloors(DbContext& db) {
    auto rows = executeQuery(db.get(), "SELECT f.id, f.floor_code, f.name, b.name FROM floors f JOIN buildings b ON f.building_id = b.id ORDER BY f.id");
    if (rows.empty()) { cout << ">> Chưa có Tầng nào.\n"; return; }
    cout << "\n--- DANH SÁCH CÁC TẦNG ---\n";
    cout << "ID | Mã tầng | Tên tầng | Thuộc tòa nhà\n";
    for (const auto& r : rows)
        cout << r[0] << " | " << r[1] << " | " << r[2] << " | " << r[3] << "\n";
}

void listRooms(DbContext& db) {
    auto rows = executeQuery(db.get(), "SELECT r.id, r.room_code, r.name, f.name FROM rooms r JOIN floors f ON r.floor_id = f.id ORDER BY r.id");
    if (rows.empty()) { cout << ">> Chưa có Phòng nào.\n"; return; }
    cout << "\n--- DANH SÁCH CÁC PHÒNG ---\n";
    cout << "ID | Mã phòng | Tên phòng | Thuộc tầng\n";
    for (const auto& r : rows)
        cout << r[0] << " | " << r[1] << " | " << r[2] << " | " << r[3] << "\n";
}

void simulateComponent(IComponent* component, const Environment& env, int hour,
                       double& totalKwh) {
    if (!component) return;

    if (component->getKind() == "Device") {
        Device* device = dynamic_cast<Device*>(component);
        if (!device) return;

        EnergyResult result = EnergyCalculator::calculateDevice(
            device->getName(), device->getAttributes(), env, hour
        );
        cout << left << setw(22) << result.deviceName
             << setw(12) << (result.isOn ? "ON" : "OFF")
             << right << setw(12) << fixed << setprecision(3) << result.powerW
             << setw(14) << result.energyKwh << "\n";
        totalKwh += result.energyKwh;
        return;
    }

    CompositeComponent* composite = dynamic_cast<CompositeComponent*>(component);
    if (composite) {
        for (IComponent* child : composite->getChildren())
            simulateComponent(child, env, hour, totalKwh);
    }
}

void runCalculationSimulation(DbContext& db) {
    int buildingId = getIntInput("Nhập ID tòa nhà: ");
    Building* building = loadBuildingFromDB(db, buildingId);
    if (!building) {
        cout << ">> Không tìm thấy tòa nhà.\n";
        return;
    }

    int hour = getIntInput("Nhập giờ cần mô phỏng (0-23): ");
    if (hour < 0 || hour > 23) {
        cout << ">> Giờ phải nằm trong khoảng 0-23.\n";
        delete building;
        return;
    }

    Environment env;
    env.occupancy = getIntInput("Số người đang có mặt trong phòng (mô hình tổng cho tòa nhà): ");
    env.roomTemperature = getDoubleInput("Nhiệt độ trong phòng (°C): ");
    env.outdoorTemperature = getDoubleInput("Nhiệt độ ngoài trời (°C): ");
    env.humidity = getDoubleInput("Độ ẩm (%): ");
    env.naturalLight = getDoubleInput("Ánh sáng tự nhiên (%): ");

    cout << "\n=== KẾT QUẢ CALCULATION TẠI " << hour << ":00 ===\n";
    cout << left << setw(22) << "Thiết bị"
         << setw(12) << "Trạng thái"
         << right << setw(12) << "Công suất W"
         << setw(14) << "Điện năng kWh" << "\n";
    cout << string(60, '-') << "\n";

    double totalKwh = 0.0;
    simulateComponent(building, env, hour, totalKwh);
    cout << string(60, '-') << "\n";
    cout << "Tổng điện năng tiêu thụ trong giờ " << hour
         << ": " << fixed << setprecision(3) << totalKwh << " kWh\n";
    cout << "Lưu ý: đây là mô hình tính cho 1 giờ; các hệ số môi trường hiện được mô phỏng đơn giản.\n";
    delete building;
}

int main() {
    try {
        DbContext db;
        int choice;
        while (true) {
            cout << "\n========================================\n";
            cout << "   HỆ THỐNG QUẢN LÝ NĂNG LƯỢNG (EMS)\n";
            cout << "========================================\n";
            cout << "1. Xem báo cáo tiêu thụ điện\n";
            cout << "2. Thêm Tòa nhà mới\n";
            cout << "3. Thêm Tầng mới\n";
            cout << "4. Thêm Phòng mới\n";
            cout << "5. Thêm Thiết bị mới\n";
            cout << "6. Mô phỏng Calculation theo giờ\n";
            cout << "0. Thoát\n";
            cout << "----------------------------------------\n";
            choice = getIntInput("Mời bạn chọn: ");
            if (choice == 0) break;
            if (choice == 1) {
                int b_id = getIntInput("Nhập ID Tòa nhà: ");
                Building* building = loadBuildingFromDB(db, b_id);
                if (!building) cout << ">> Không tìm thấy Tòa nhà.\n";
                else {
                    cout << "\n=== ENERGY HIERARCHY ===\n";
                    building->printTree(0);
                    cout << "Total monthly kWh: " << building->getMonthlyKwh() << "\n";
                    delete building;
                }
            } else if (choice == 2) {
                string code = getStringInput("Mã tòa nhà: ");
                string name = getStringInput("Tên tòa nhà: ");
                int new_id = insertBuilding(db, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
            } else if (choice == 3) {
                int b_id = getIntInput("Nhập ID Tòa nhà chứa tầng: ");
                string code = getStringInput("Mã tầng: ");
                string name = getStringInput("Tên tầng: ");
                int new_id = insertFloor(db, b_id, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
            } else if (choice == 4) {
                listFloors(db);
                int f_id = getIntInput(">> Nhập ID Tầng: ");
                string code = getStringInput("Mã phòng: ");
                string name = getStringInput("Tên phòng: ");
                int new_id = insertRoom(db, f_id, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
            } else if (choice == 5) {
                listRooms(db);
                int r_id = getIntInput(">> Nhập ID Phòng: ");
                string dev_name = getStringInput("Tên thiết bị: ");
                json attrs;
                attrs["device_type"] = getStringInput("Loại thiết bị: ");
                attrs["rated_power_w"] = getDoubleInput("Công suất (W): ");
                attrs["usage_hours_per_day"] = getDoubleInput("Số giờ/ngày: ");
                attrs["usage_days_per_week"] = getDoubleInput("Số ngày/tuần: ");
                attrs["load_factor"] = getDoubleInput("Hệ số tải (0.1-1.0): ");
                insertDevice(db, r_id, dev_name, attrs);
            } else if (choice == 6) {
                runCalculationSimulation(db);
            }
        }
    } catch (const exception& e) {
        cerr << "Lỗi: " << e.what() << "\n";
        return 1;
    }
    return 0;
}