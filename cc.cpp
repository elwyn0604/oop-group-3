#include <iostream>
#include <memory>
#include <string>

class EnergyComponent {
public:
    virtual ~EnergyComponent() = default;
    virtual double calculateEnergy() const = 0;
    virtual std::string getDescription() const = 0;
};

class BasicDevice : public EnergyComponent {
private:
    std::string name;
    double basePower;

public:
    BasicDevice(std::string n, double power) : name(n), basePower(power) {}

    double calculateEnergy() const override {
        return basePower;
    }

    std::string getDescription() const override {
        return name;
    }
};

class EnergyDecorator : public EnergyComponent {
protected:
    std::shared_ptr<EnergyComponent> component;

public:
    EnergyDecorator(std::shared_ptr<EnergyComponent> comp) 
        : component(comp) {}

    double calculateEnergy() const override {
        return component ? component->calculateEnergy() : 0.0;
    }

    std::string getDescription() const override {
        return component ? component->getDescription() : "";
    }
};

class TemperatureDecorator : public EnergyDecorator {
private:
    double temperature;

public:
    TemperatureDecorator(std::shared_ptr<EnergyComponent> comp, double temp)
        : EnergyDecorator(comp), temperature(temp) {}

    double calculateEnergy() const override {
        double baseEnergy = EnergyDecorator::calculateEnergy();
        double factor = 1.0;
        if (temperature > 30.0) {
            factor += (temperature - 30.0) * 0.05;
        }
        return baseEnergy * factor;
    }

    std::string getDescription() const override {
        return EnergyDecorator::getDescription() + " + [Nhiet do: " + std::to_string((int)temperature) + "C]";
    }
};

class OccupancyDecorator : public EnergyDecorator {
private:
    int peopleCount;

public:
    OccupancyDecorator(std::shared_ptr<EnergyComponent> comp, int count)
        : EnergyDecorator(comp), peopleCount(count) {}

    double calculateEnergy() const override {
        double baseEnergy = EnergyDecorator::calculateEnergy();
        double factor = 1.0 + (peopleCount * 0.02);
        return baseEnergy * factor;
    }

    std::string getDescription() const override {
        return EnergyDecorator::getDescription() + " + [So nguoi: " + std::to_string(peopleCount) + "]";
    }
};

int main() {
    std::shared_ptr<EnergyComponent> room = std::make_shared<BasicDevice>("Phong 101", 1000.0);

    std::cout << "=== TRANG THAI BAN DAU ===" << std::endl;
    std::cout << "Mo ta: " << room->getDescription() << std::endl;
    std::cout << "Tieu thu: " << room->calculateEnergy() << " W\n\n";

    room = std::make_shared<TemperatureDecorator>(room, 35.0);
    room = std::make_shared<OccupancyDecorator>(room, 10);

    std::cout << "=== SAU KHI AP DUNG DECORATOR ===" << std::endl;
    std::cout << "Mo ta: " << room->getDescription() << std::endl;
    std::cout << "Tong tieu thu: " << room->calculateEnergy() << " W" << std::endl;

    return 0;
}