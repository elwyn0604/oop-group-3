#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#incl   ude <limits>
#include <iomanip>

// Thư viện mã nguồn mở (Open-source) để xử lý JSON trong C++
#include <nlohmann/json.hpp> 

// Thư viện C API của MySQL
#include <mysql.h> 

using namespace std;
using json = nlohmann::json;

// =====================================================
// THÔNG TIN KẾT NỐI MYSQL
// =====================================================
static const string DB_HOST = "127.0.0.1";
static const string DB_USER = "root";
static const string DB_PASSWORD = "Hung28112007@";
static const string DB_SCHEMA = "energy_management_db";

// =====================================================
// CÁC HÀM HỖ TRỢ NHẬP LIỆU (Giữ nguyên của bạn)
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

// =====================================================
// 1. INTERFACE (Tính Đa Hình - Polymorphism)
// =====================================================
class IComponent {
public:
    virtual ~IComponent() {} // Virtual Destructor
    virtual int getId() = 0;
    virtual string getName() = 0;
    virtual string getKind() = 0;
    virtual double getDailyKwh() = 0;
    virtual double getMonthlyKwh() = 0;
    virtual void addChild(IComponent* child) {} // Mặc định rỗng
    virtual void printTree(int level) = 0;
};

// =====================================================
// 2. COMPOSITE BASE CLASS (Tính Kế Thừa)
// =====================================================
class CompositeComponent : public IComponent {
protected:
    int id;
    string name;
    vector<IComponent*> children; // Dùng vector con trỏ cơ bản

public:
    CompositeComponent(int _id, string _name) : id(_id), name(_name) {}

    // Hàm hủy đệ quy: Tự động giải phóng bộ nhớ các con khi xóa cha
    ~CompositeComponent() {
        for (int i = 0; i < children.size(); i++) {
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
        for (int i = 0; i < children.size(); i++) {
            total += children[i]->getDailyKwh();
        }
        return total;
    }

    double getMonthlyKwh() override {
        double total = 0.0;
        for (int i = 0; i < children.size(); i++) {
            total += children[i]->getMonthlyKwh();
        }
        return total;
    }

    void printTree(int level) override {
        string indent(level * 2, ' ');
        cout << fixed << setprecision(2);
        cout << indent << "[" << getKind() << "] " << name 
             << " - Daily: " << getDailyKwh() << " kWh"
             << " - Monthly: " << getMonthlyKwh() << " kWh\n";
        
        for (int i = 0; i < children.size(); i++) {
            children[i]->printTree(level + 1);
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
public:
    Room(int id, string code, string name) : CompositeComponent(id, name) {}
    string getKind() override { return "Room"; }
};

// =====================================================
// 3. DEVICE - LEAF NODE
// =====================================================
class Device : public IComponent {
private:
    int id;
    string name;
    json attributes; // Dùng thư viện JSON

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
// 4. XỬ LÝ MYSQL (C API CƠ BẢN)
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

int safeGetInt(const string& s) {
    if (s.empty()) return 0;
    try { return stoi(s); } catch (...) { return 0; }
}

// =====================================================
// 5. TẢI DỮ LIỆU TỪ DB (Dùng vòng lặp lồng nhau - Rất sinh viên)
// =====================================================
Building* loadBuildingFromDB(DbContext& db, int buildingId) {
    string sqlB = "SELECT id, building_code, name FROM buildings WHERE id = " + to_string(buildingId);
    auto b_rows = executeQuery(db.get(), sqlB);
    if (b_rows.empty()) return nullptr;

    Building* building = new Building(safeGetInt(b_rows[0][0]), b_rows[0][1], b_rows[0][2]);

    // Load Floors
    string sqlF = "SELECT id, floor_code, name FROM floors WHERE building_id = " + to_string(buildingId);
    auto f_rows = executeQuery(db.get(), sqlF);
    
    for (const auto& f : f_rows) {
        int floorId = safeGetInt(f[0]);
        Floor* floor = new Floor(floorId, f[1], f[2]);

        // Load Rooms
        string sqlR = "SELECT id, room_code, name FROM rooms WHERE floor_id = " + to_string(floorId);
        auto r_rows = executeQuery(db.get(), sqlR);

        for (const auto& r : r_rows) {
            int roomId = safeGetInt(r[0]);
            Room* room = new Room(roomId, r[1], r[2]);

            // Load Devices
            string sqlD = "SELECT id, device_name, attributes FROM devices WHERE room_id = " + to_string(roomId);
            auto d_rows = executeQuery(db.get(), sqlD);

            for (const auto& d : d_rows) {
                Device* device = new Device(safeGetInt(d[0]), d[1], d[2]);
                room->addChild(device);
            }
            floor->addChild(room);
        }
        building->addChild(floor);
    }
    return building;
}

// =====================================================
// 6. CÁC HÀM THÊM MỚI (INSERT)
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

int insertRoom(DbContext& db, int f_id, const string& code, const string& name) {
    string sql = "INSERT INTO rooms (floor_id, room_code, name) VALUES (" + to_string(f_id) + ", '" + db.escape(code) + "', '" + db.escape(name) + "')";
    if (mysql_query(db.get(), sql.c_str())) return -1;
    return (int)mysql_insert_id(db.get());
}

void insertDevice(DbContext& db, int r_id, const string& name, const json& attrs) {
    string sql = "INSERT INTO devices (room_id, device_name, attributes) VALUES (" + to_string(r_id) + ", '" + db.escape(name) + "', '" + db.escape(attrs.dump()) + "')";
    if (mysql_query(db.get(), sql.c_str())) {
        cout << "Lỗi thêm Device\n";
    } else {
        cout << ">> Đã thêm thiết bị thành công!\n";
    }
}

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
    auto rows = executeQuery(db.get(), "SELECT r.id, r.room_code, r.name, f.name FROM rooms r JOIN floors f ON r.floor_id = f.id ORDER BY r.id");
    if (rows.empty()) { cout << ">> Chưa có Phòng nào.\n"; return; }
    cout << "\n--- DANH SÁCH CÁC PHÒNG ---\n";
    cout << "ID | Mã phòng | Tên phòng | Thuộc tầng\n";
    for (const auto& r : rows) {
        cout << r[0] << " | " << r[1] << " | " << r[2] << " | " << r[3] << "\n";
    }
}

// =====================================================
// 7. HÀM MAIN - MENU TƯƠNG TÁC
// =====================================================
int main() {
    try {
        DbContext db;
        int choice;
        
        while (true) {
            cout << "\n========================================\n";
            cout << "   HỆ THỐNG QUẢN LÝ NĂNG LƯỢNG (EMS)    \n";
            cout << "========================================\n";
            cout << "1. Xem báo cáo tiêu thụ điện\n";
            cout << "2. Thêm Tòa nhà mới\n";
            cout << "3. Thêm Tầng mới\n";
            cout << "4. Thêm Phòng mới\n";
            cout << "5. Thêm Thiết bị mới\n";
            cout << "0. Thoát\n";
            cout << "----------------------------------------\n";
            
            choice = getIntInput("Mời bạn chọn: ");

            if (choice == 0) break;

            if (choice == 1) {
                int b_id = getIntInput("Nhập ID Tòa nhà: ");
                Building* building = loadBuildingFromDB(db, b_id);
                if (!building) {
                    cout << ">> Không tìm thấy Tòa nhà.\n";
                } else {
                    cout << "\n=== ENERGY HIERARCHY ===\n";
                    building->printTree(0);
                    cout << "Total monthly kWh: " << building->getMonthlyKwh() << "\n";
                    delete building; // Giải phóng bộ nhớ (Destructor đệ quy)
                }
            } 
            else if (choice == 2) {
                string code = getStringInput("Mã tòa nhà: ");
                string name = getStringInput("Tên tòa nhà: ");
                int new_id = insertBuilding(db, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
            } 
            else if (choice == 3) {
                int b_id = getIntInput("Nhập ID Tòa nhà chứa tầng: ");
                string code = getStringInput("Mã tầng: ");
                string name = getStringInput("Tên tầng: ");
                int new_id = insertFloor(db, b_id, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
            } 
            else if (choice == 4) {
                listFloors(db);
                int f_id = getIntInput(">> Nhập ID Tầng: ");
                string code = getStringInput("Mã phòng: ");
                string name = getStringInput("Tên phòng: ");
                int new_id = insertRoom(db, f_id, code, name);
                if (new_id > 0) cout << ">> Tạo thành công! ID: " << new_id << "\n";
            } 
            else if (choice == 5) {
                listRooms(db); // Thêm hàm listRooms để dễ chọn ID
                int r_id = getIntInput(">> Nhập ID Phòng: ");
                string dev_name = getStringInput("Tên thiết bị: ");
                
                json attrs;
                attrs["device_type"] = getStringInput("Loại thiết bị: ");
                attrs["rated_power_w"] = getDoubleInput("Công suất (W): ");
                attrs["usage_hours_per_day"] = getDoubleInput("Số giờ/ngày: ");
                attrs["usage_days_per_week"] = getDoubleInput("Số ngày/tuần: ");
                attrs["load_factor"] = getDoubleInput("Hệ số tải (0.1-1.0): ");

                insertDevice(db, r_id, dev_name, attrs);
            } 
        }

    } catch (const exception& e) {
        cerr << "Lỗi: " << e.what() << "\n";
        return 1;
    }

    return 0;
}