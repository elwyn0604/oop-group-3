#include <iostream>
#include <memory>
#include <string>
#include <iomanip>

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
        return name + (isON ? " [Trạng thái: ON]" : " [Trạng thái: OFF]");
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
                          std::string type, int people, double temp, double hum, double lux)
        : EnergyDecorator(comp), deviceType(type), 
          peopleCount(people), temperature(temp), humidity(hum), lightLux(lux) {}

    void updateStatus() override {
        EnergyDecorator::updateStatus();

        auto rawDev = std::dynamic_pointer_cast<ConcreteDevice>(component);
        if (!rawDev) return;

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
        return EnergyDecorator::getDescription() + " + [Độ cũ: " + std::to_string((int)wearPercentage) + "%]";
    }
};

int main() {
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
    auto acDev = std::make_shared<ConcreteDevice>("AC (Dieu hoa)", 1200.0);
    auto compDev = std::make_shared<ConcreteDevice>("Computer (May tinh)", 300.0);

    auto smartLight = std::make_shared<SmartDeviceController>(lightDev, "Light", people, temp, humidity, lux);
    auto smartAC = std::make_shared<SmartDeviceController>(acDev, "AC", people, temp, humidity, lux);
    auto smartComp = std::make_shared<SmartDeviceController>(compDev, "Computer", people, temp, humidity, lux);

    std::shared_ptr<EnergyComponent> finalAC = std::make_shared<AgeDecorator>(smartAC, 20.0);

    smartLight->updateStatus();
    smartAC->updateStatus();
    smartComp->updateStatus();

    double eLight = smartLight->calculateEnergy(timeInHours);
    double eAC = finalAC->calculateEnergy(timeInHours);
    double eComp = smartComp->calculateEnergy(timeInHours);
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

    return 0;
}