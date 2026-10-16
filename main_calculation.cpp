// #define CALC_STANDALONE  // ← Bỏ dấu // nếu compile ĐỘC LẬP

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

// =====================================================
// PHẦN 1: STRUCTS MỚI (Không trùng với main.cpp)
// =====================================================

struct CalcEnvironment {
    int occupancy       = 0;
    double temperatureC = 25.0;
    double humidity     = 50.0;
    double lightLux     = 300.0;
    int hour            = 0;
};

struct CalcEnergyResult {
    string deviceName;
    bool   isOn         = false;
    double ratedPowerW  = 0.0;
    double smartPowerW  = 0.0;
    double energyKwh    = 0.0;
    string note;
};

// =====================================================
// PHẦN 2: ENERGY CALCULATOR (Không trùng với main.cpp)
// =====================================================

class EnergyCalculator {
public:
    // ---------- Đọc JSON an toàn ----------
    static double getNum(const nlohmann::json& a, const string& k, double def) {
        if (!a.contains(k)) return def;
        if (a[k].is_number()) return a[k].get<double>();
        if (a[k].is_string()) { try { return stod(a[k].get<string>()); } catch (...) {} }
        return def;
    }

    static string getStr(const nlohmann::json& a, const string& k, const string& def) {
        if (!a.contains(k)) return def;
        if (a[k].is_string()) return a[k].get<string>();
        return def;
    }

    static bool getBool(const nlohmann::json& a, const string& k, bool def) {
        if (!a.contains(k)) return def;
        if (a[k].is_boolean()) return a[k].get<bool>();
        if (a[k].is_number()) return a[k].get<int>() != 0;
        if (a[k].is_string()) {
            string v = a[k].get<string>();
            return (v == "true" || v == "1" || v == "on" || v == "yes");
        }
        return def;
    }

    // ---------- Kiểm tra thiết bị BẬT tại giờ ----------
    static bool isOnAt(const nlohmann::json& a, int hour) {
        hour = ((hour % 24) + 24) % 24;

        // Ưu tiên schedule dạng mảng [{start,end},...]
        if (a.contains("schedule") && a["schedule"].is_array()) {
            for (const auto& slot : a["schedule"]) {
                if (!slot.is_object()) continue;
                int s = slot.value("start", 0);
                int e = slot.value("end", 0);
                if (s == e) continue;
                if (s < e) { if (hour >= s && hour < e) return true; }
                else       { if (hour >= s || hour < e) return true; }
            }
            return false;
        }

        // Fallback: usage_hours_per_day + start_hour
        double usage = getNum(a, "usage_hours_per_day", 24.0);
        if (usage <= 0) return false;
        if (usage >= 24) return true;
        int start = ((int)getNum(a, "start_hour", 0.0) % 24 + 24) % 24;
        int dur   = (int)usage;
        if (start + dur <= 24) return hour >= start && hour < start + dur;
        return hour >= start || hour < (start + dur) % 24;
    }

    // ---------- Kiểm tra loại thiết bị ----------
    static bool isType(const string& type, const vector<string>& keys) {
        string lo = type;
        for (char& c : lo) c = tolower(c);
        for (const string& k : keys) {
            string kl = k;
            for (char& c : kl) c = tolower(c);
            if (lo.find(kl) != string::npos) return true;
        }
        return false;
    }

    // ---------- TÍNH CÔNG SUẤT THÔNG MINH ----------
    static double calcSmartPower(
        double ratedPower,
        const nlohmann::json& attrs,
        const CalcEnvironment& env,
        string& noteOut
    ) {
        double power = ratedPower;
        noteOut = "";
        if (power <= 0) return 0;

        string type = getStr(attrs, "device_type", "");

        // 1. Tắt khi vắng người
        if (getBool(attrs, "occupancy_sensitive", false)) {
            if (env.occupancy <= 0) {
                noteOut = "TAT (vang nguoi)";
                return 0;
            }
            if (env.occupancy < 3) {
                power *= 0.6;
                noteOut = "Giam 40% (it nguoi)";
            }
        }

        // 2. Đèn: tắt/giảm khi đủ ánh sáng tự nhiên
        if (isType(type, {"light", "den", "đèn"})) {
            if (getBool(attrs, "daylight_sensitive", false)) {
                double th = getNum(attrs, "daylight_threshold", 500.0);
                if (env.lightLux >= th) {
                    noteOut = "TAT (du anh sang)";
                    return 0;
                }
                if (env.lightLux >= th * 0.5) {
                    power *= 0.5;
                    noteOut = "Giam 50% (sang vua)";
                }
            }
        }

        // 3. Điều hòa: +5% mỗi độ trên 25°C
        if (isType(type, {"ac", "air conditioner", "dieu hoa", "điều hòa", "may lanh"})) {
            if (env.temperatureC > 25.0) {
                double extra = (env.temperatureC - 25.0) * 0.05;
                power *= (1.0 + extra);
                noteOut = "Tang " + to_string((int)(extra * 100)) + "% (nong)";
            } else if (env.temperatureC < 22.0) {
                double red = (22.0 - env.temperatureC) * 0.02;
                power *= (1.0 - red);
                noteOut = "Giam (mat)";
            }
        }

        // 4. Độ ẩm: tăng khi > 60%
        if (getBool(attrs, "humidity_sensitive", false)) {
            if (env.humidity > 60.0) {
                power *= 1.0 + (env.humidity - 60.0) * 0.005;
                noteOut = "Tang (am)";
            }
        }

        // 5. Load factor
        double lf = getNum(attrs, "load_factor", 1.0);
        lf = max(0.0, min(1.0, lf));
        power *= lf;

        return max(0.0, power);
    }

    // ---------- Tính kWh ----------
    static double calcKwh(double powerW, double hours = 1.0) {
        if (powerW < 0 || hours < 0) return 0;
        return powerW / 1000.0 * hours;
    }

    // ---------- Tính đầy đủ cho 1 thiết bị ----------
    static CalcEnergyResult calcDevice(
        const string& name,
        const nlohmann::json& attrs,
        const CalcEnvironment& env
    ) {
        CalcEnergyResult r;
        r.deviceName  = name;
        r.ratedPowerW = getNum(attrs, "rated_power_w", 0.0);
        r.isOn        = isOnAt(attrs, env.hour);

        if (!r.isOn) {
            r.note = "Ngoai lich hoat dong";
            return r;
        }

        r.smartPowerW = calcSmartPower(r.ratedPowerW, attrs, env, r.note);
        r.energyKwh   = calcKwh(r.smartPowerW, 1.0);
        if (r.note.empty()) r.note = "Binh thuong";
        return r;
    }
};

// =====================================================
// PHẦN 3: HÀM TÍCH HỢP
// Từ main.cpp gọi: runCalculationSimulation(db.get());
// =====================================================

void runCalculationSimulation(MYSQL* conn) {
    // --- Nhập liệu trực tiếp (không phụ thuộc helper của main.cpp) ---
    int buildingId;
    cout << "\nNhap ID Toa nha: ";
    cin >> buildingId;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    CalcEnvironment env;
    cout << "Nhap gio can mo phong (0-23): ";
    cin >> env.hour;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');
    env.hour = max(0, min(23, env.hour));

    cout << "So nguoi trong phong: ";
    cin >> env.occupancy;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    cout << "Nhiet do phong (do C): ";
    cin >> env.temperatureC;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    cout << "Do am (%): ";
    cin >> env.humidity;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    cout << "Anh sang tu nhien (Lux): ";
    cin >> env.lightLux;
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    // --- Query tất cả device trong tòa nhà ---
    string sql =
        "SELECT d.id, d.device_name, d.attributes, r.name AS room_name "
        "FROM devices d "
        "JOIN rooms r ON d.room_id = r.id "
        "JOIN floors f ON r.floor_id = f.id "
        "WHERE f.building_id = " + to_string(buildingId) +
        " ORDER BY f.id, r.id, d.id";

    if (mysql_query(conn, sql.c_str())) {
        cout << ">> Loi SQL: " << mysql_error(conn) << "\n";
        return;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) {
        cout << ">> Khong co du lieu.\n";
        return;
    }

    // --- In kết quả ---
    cout << "\n=== KET QUA MO PHONG tai " << env.hour << ":00 ===\n";
    cout << string(100, '-') << "\n";
    cout << left
         << setw(25) << "Thiet bi"
         << setw(12) << "Phong"
         << setw(8)  << "Trang thai"
         << right
         << setw(12) << "Dinh muc(W)"
         << setw(14) << "Thuc te(W)"
         << setw(12) << "kWh"
         << "  | Ghi chu\n";
    cout << string(100, '-') << "\n";

    double totalKwh = 0.0;
    int deviceCount = 0;
    MYSQL_ROW row;

    while ((row = mysql_fetch_row(res))) {
        string devName  = row[1] ? row[1] : "";
        string roomName = row[3] ? row[3] : "";
        string attrStr  = row[2] ? row[2] : "{}";

        nlohmann::json attrs = nlohmann::json::parse(attrStr, nullptr, false);
        if (!attrs.is_object()) attrs = nlohmann::json::object();

        CalcEnergyResult r = EnergyCalculator::calcDevice(devName, attrs, env);
        totalKwh += r.energyKwh;
        deviceCount++;

        cout << left
             << setw(25) << devName.substr(0, 24)
             << setw(12) << roomName.substr(0, 11)
             << setw(8)  << (r.isOn ? "BAT" : "TAT")
             << right << fixed
             << setw(12) << setprecision(1) << r.ratedPowerW
             << setw(14) << setprecision(1) << r.smartPowerW
             << setw(12) << setprecision(3) << r.energyKwh
             << "  | " << r.note << "\n";
    }

    mysql_free_result(res);

    cout << string(100, '-') << "\n";
    cout << ">> Tong: " << deviceCount << " thiet bi | "
         << fixed << setprecision(3) << totalKwh << " kWh trong 1 gio\n";
    cout << ">> Cong thuc: Den tat khi du sang | AC +5%/do tren 25C"
         << " | Tat khi vang nguoi | Tang khi am cao\n";
}

// =====================================================
// PHẦN 4: STANDALONE MODE
// Chỉ compile khi #define CALC_STANDALONE được bật.
// Khi paste vào main.cpp, phần này tự động bị bỏ qua.
// =====================================================
#ifdef CALC_STANDALONE

static const string CALC_DB_HOST = "127.0.0.1";
static const string CALC_DB_USER = "root";
static const string CALC_DB_PASS = "Minhdat1";
static const string CALC_DB_NAME = "energy_management_db";

int main() {
    cout << "=== CALCULATION MODULE (Standalone) ===\n";

    MYSQL* conn = mysql_init(nullptr);
    if (!conn) {
        cerr << ">> Loi khoi tao MySQL.\n";
        return 1;
    }

    if (!mysql_real_connect(
        conn,
        CALC_DB_HOST.c_str(),
        CALC_DB_USER.c_str(),
        CALC_DB_PASS.c_str(),
        CALC_DB_NAME.c_str(),
        3306, nullptr, 0
    )) {
        cerr << ">> Loi ket noi: " << mysql_error(conn) << "\n";
        mysql_close(conn);
        return 1;
    }
    mysql_set_character_set(conn, "utf8mb4");
    cout << ">> Ket noi MySQL thanh cong.\n";

    while (true) {
        cout << "\n--- CALCULATION MENU ---\n";
        cout << "1. Mo phong tieu thu theo gio\n";
        cout << "0. Thoat\n";
        int ch;
        cout << "Chon: ";
        cin >> ch;
        cin.ignore(numeric_limits<streamsize>::max(), '\n');

        if (ch == 0) break;
        if (ch == 1) {
            runCalculationSimulation(conn);
        } else {
            cout << ">> Khong hop le.\n";
        }
    }

    mysql_close(conn);
    return 0;
}

#endif // CALC_STANDALONE

//         runCalculationSimulation(db.get());
//     }
//
// Xong! Compile và chạy.
// =====================================================
