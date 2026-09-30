#include "Variable.hpp"

#include <algorithm>
#include <limits>
#include <vector>

const char* Variable::types[8] = {"unknown",
								  "uint8_t",
								  "int8_t",
								  "uint16_t",
								  "int16_t",
								  "uint32_t",
								  "int32_t",
								  "float"};

const char* Variable::highLevelTypes[5] = {"-",
										   "signed fixed point",
										   "unsigned fixed point",
										   "enum",
										   "custom enum"};

Variable::Variable(std::string name) : name(name)
{
	name.reserve(100);
}

Variable::Variable(std::string name, Variable::Type type, double value) : name(name), type(type), value(value)
{
	name.reserve(100);
}

void Variable::setType(Type type)
{
	this->type = type;
}

Variable::Type Variable::getType() const
{
	return type;
}

std::string Variable::getTypeStr() const
{
	return std::string(types[static_cast<uint8_t>(type)]);
}

void Variable::setRawValue(uint32_t rawValue)
{
	this->rawValue = (rawValue >> shift) & mask;
}

void Variable::setValue(double val)
{
	value = val;
}

double Variable::getValue() const
{
	return value;
}

void Variable::setAddress(uint32_t addr)
{
	address = addr;
}
uint32_t Variable::getAddress() const
{
	return address;
}

std::string Variable::getName() const
{
	return name;
}

void Variable::rename(const std::string& newName)
{
	name = newName;

	if (!isTrackedNameDifferent)
		trackedName = newName;
}

void Variable::setColor(float r, float g, float b, float a)
{
	color.r = r;
	color.g = g;
	color.b = b;
	color.a = a;
}

void Variable::setColor(uint32_t AaBbGgRr)
{
	using u8 = std::numeric_limits<uint8_t>;
	color.r = static_cast<float>((AaBbGgRr & 0x000000ff) / static_cast<float>(u8::max()));
	color.g = static_cast<float>(((AaBbGgRr & 0x0000ff00) >> 8) / static_cast<float>(u8::max()));
	color.b = static_cast<float>(((AaBbGgRr & 0x00ff0000) >> 16) / static_cast<float>(u8::max()));
	color.a = static_cast<float>(((AaBbGgRr & 0xff000000) >> 24) / static_cast<float>(u8::max()));
}

Variable::Color& Variable::getColor()
{
	return color;
}

uint32_t Variable::getColorU32() const
{
	using u8 = std::numeric_limits<uint8_t>;
	uint32_t a = u8::max() * color.a;
	uint32_t r = u8::max() * color.r;
	uint32_t g = u8::max() * color.g;
	uint32_t b = u8::max() * color.b;

	return static_cast<uint32_t>((a << 24) | (b << 16) | (g << 8) | r);
}

bool Variable::getIsFound() const
{
	if (shouldUpdateFromElf)
		return isFound;
	return true;
}

void Variable::setIsFound(bool found)
{
	isFound = found;
}

uint8_t Variable::getSize()
{
	switch (type)
	{
		case Type::U8:
		case Type::I8:
			return 1;
		case Type::U16:
		case Type::I16:
			return 2;
		case Type::U32:
		case Type::I32:
		case Type::F32:
			return 4;
		default:
			return 1;
	}
}

double Variable::transformToDouble()
{
	uint32_t size = getSize();

	if (HighLevelType::SIGNEDFRAC == highLevelType)
	{
		if (fractional.baseVariable != nullptr)
			fractional.base = fractional.baseVariable->getValue();

		switch (size)
		{
			case 1:
			{
				int8_t temp = rawValue & 0xff;
				value = (static_cast<double>(temp) / (1 << (fractional.fractionalBits))) * fractional.base;
				break;
			}
			case 2:
			{
				int16_t temp = rawValue & 0xffff;
				value = (static_cast<double>(temp) / (1 << (fractional.fractionalBits))) * fractional.base;
				break;
			}
			case 4:
			{
				int32_t temp = rawValue;
				value = (static_cast<double>(temp) / (1 << (fractional.fractionalBits))) * fractional.base;
				break;
			}
		}
		return value;
	}
	else if (HighLevelType::UNSIGNEDFRAC == highLevelType)
	{
		if (fractional.baseVariable != nullptr)
			fractional.base = fractional.baseVariable->getValue();

		switch (size)
		{
			case 1:
			{
				uint8_t temp = rawValue & 0xff;
				value = (static_cast<double>(temp) / (1 << fractional.fractionalBits)) * fractional.base;
				break;
			}
			case 2:
			{
				uint16_t temp = rawValue & 0xffff;
				value = (static_cast<double>(temp) / (1 << fractional.fractionalBits)) * fractional.base;
				break;
			}
			case 4:
			{
				uint32_t temp = rawValue;
				value = (static_cast<double>(temp) / (1 << fractional.fractionalBits)) * fractional.base;
				break;
			}
		}
		return value;
	}

	switch (type)
	{
		case Variable::Type::U8:
			value = static_cast<double>(*reinterpret_cast<uint8_t*>(&rawValue));
			break;
		case Variable::Type::I8:
			value = static_cast<double>(*reinterpret_cast<int8_t*>(&rawValue));
			break;
		case Variable::Type::U16:
			value = static_cast<double>(*reinterpret_cast<uint16_t*>(&rawValue));
			break;
		case Variable::Type::I16:
			value = static_cast<double>(*reinterpret_cast<int16_t*>(&rawValue));
			break;
		case Variable::Type::U32:
			value = static_cast<double>(*reinterpret_cast<uint32_t*>(&rawValue));
			break;
		case Variable::Type::I32:
			value = static_cast<double>(*reinterpret_cast<int32_t*>(&rawValue));
			break;
		case Variable::Type::F32:
			value = static_cast<double>(*reinterpret_cast<float*>(&rawValue));
			break;
		default:
			value = static_cast<double>(*reinterpret_cast<uint32_t*>(&rawValue));
			break;
	}

	return value;
}

uint32_t Variable::getRawFromDouble(double value)
{
	if (isFractional())
		return static_cast<uint32_t>((value / fractional.base) * (1 << fractional.fractionalBits));

	switch (getType())
	{
		case Variable::Type::U8:
			return static_cast<uint8_t>(value);
		case Variable::Type::I8:
			return static_cast<int8_t>(value);
		case Variable::Type::U16:
			return static_cast<uint16_t>(value);
		case Variable::Type::I16:
			return static_cast<int16_t>(value);
		case Variable::Type::U32:
			return static_cast<uint32_t>(value);
		case Variable::Type::I32:
			return static_cast<int32_t>(value);
		case Variable::Type::F32:
		{
			float valf = static_cast<float>(value);
			return *reinterpret_cast<uint32_t*>(&valf);
		}
		default:
			return 0;
	}
}

bool Variable::getShouldUpdateFromElf() const
{
	return shouldUpdateFromElf;
}

void Variable::setShouldUpdateFromElf(bool shouldUpdateFromElf)
{
	this->shouldUpdateFromElf = shouldUpdateFromElf;
}

bool Variable::getIsTrackedNameDifferent() const
{
	return isTrackedNameDifferent;
}

void Variable::setIsTrackedNameDifferent(bool isDifferent)
{
	this->isTrackedNameDifferent = isDifferent;
}

std::string Variable::getTrackedName() const
{
	return trackedName;
}

void Variable::setTrackedName(const std::string& trackedName)
{
	this->trackedName = trackedName;
}

void Variable::setShift(uint32_t shift)
{
	this->shift = shift;
}

uint32_t Variable::getShift() const
{
	return shift;
}

void Variable::setMask(uint32_t mask)
{
	this->mask = mask;
}

uint32_t Variable::getMask() const
{
	return mask;
}

void Variable::setHighLevelType(HighLevelType varType)
{
	highLevelType = varType;
}

Variable::HighLevelType Variable::getHighLevelType() const
{
	return highLevelType;
}

void Variable::setFractional(Fractional fractional)
{
	this->fractional = fractional;
}

Variable::Fractional Variable::getFractional() const
{
	return fractional;
}

bool Variable::isFractional() const
{
	return highLevelType == HighLevelType::SIGNEDFRAC || highLevelType == HighLevelType::UNSIGNEDFRAC;
}

void Variable::setEnumLabels(const std::vector<EnumLabel>& enumLabels)
{
	this->enumLabels = enumLabels;
}

std::vector<Variable::EnumLabel> Variable::getEnumLabels() const
{
	return enumLabels;
}

bool Variable::isEnum() const
{
	return highLevelType == HighLevelType::ENUM || highLevelType == HighLevelType::CUSTOM_ENUM;
}

std::string Variable::getEnumLabel(double value) const
{
	if (!isEnum())
		return "";

	/* The stored value is what the variable shows after postprocessing, so a
	   match can be made directly. A value with a fraction cannot name a label,
	   hence the truncation rather than a comparison of the doubles. */
	const int64_t asInteger = static_cast<int64_t>(value);

	for (const EnumLabel& entry : enumLabels)
	{
		if (entry.value == asInteger)
			return entry.label;
	}

	return "";
}

void Variable::setWriteLimits(bool enabled, double min, double max)
{
	writeLimitsEnabled = enabled;
	writeMin = min;
	writeMax = max;
}

bool Variable::getWriteLimitsEnabled() const
{
	return writeLimitsEnabled;
}

double Variable::getWriteMin() const
{
	return writeMin;
}

double Variable::getWriteMax() const
{
	return writeMax;
}

bool Variable::isWriteAllowed(double valueToWrite) const
{
	if (!writeLimitsEnabled)
		return true;

	return valueToWrite >= writeMin && valueToWrite <= writeMax;
}

void Variable::setIsCurrentlySampled(bool isCurrentlySampled)
{
	this->isCurrentlySampled = isCurrentlySampled;
}

bool Variable::getIsCurrentlySampled() const
{
	return isCurrentlySampled;
}