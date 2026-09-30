#ifndef __VARIABLE_HPP
#define __VARIABLE_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Variable
{
   public:
	enum class Type
	{
		UNKNOWN = 0,
		U8 = 1,
		I8 = 2,
		U16 = 3,
		I16 = 4,
		U32 = 5,
		I32 = 6,
		F32 = 7
	};

	enum class HighLevelType
	{
		NONE = 0,
		SIGNEDFRAC = 1,
		UNSIGNEDFRAC = 2,
		/* Labels come from the symbol file, so nothing has to be stored with the
		   variable. A parser that carries no enumeration information leaves the
		   value displayed as a number. */
		ENUM = 3,
		/* The user supplies the labels; they are stored with the variable and
		   travel in the project file. */
		CUSTOM_ENUM = 4,
	};

	struct Color
	{
		float r;
		float g;
		float b;
		float a;
	};

	struct Fractional
	{
		uint32_t fractionalBits = 15;
		double base = 1.0;
		Variable* baseVariable = nullptr;
	};

	/* One entry of a custom enumeration: a name for a value the variable can
	   take. Values are signed so that a variable read as int8_t can be named. */
	struct EnumLabel
	{
		std::string label;
		int64_t value = 0;

		bool operator==(const EnumLabel& other) const
		{
			return label == other.label && value == other.value;
		}
	};

	explicit Variable(std::string name);
	Variable(std::string name, Type type, double value);

	void setType(Type type);
	Type getType() const;
	std::string getTypeStr() const;

	void setRawValue(uint32_t value);
	void setValue(double val);
	double getValue() const;

	void setAddress(uint32_t addr);
	uint32_t getAddress() const;
	std::string getName();
	void rename(const std::string& newName);

	void setColor(float r, float g, float b, float a);
	void setColor(uint32_t AaBbGgRr);

	Color& getColor();
	uint32_t getColorU32() const;

	bool getIsFound() const;
	void setIsFound(bool found);

	uint8_t getSize();

	bool getShouldUpdateFromElf() const;
	void setShouldUpdateFromElf(bool shouldUpdateFromElf);

	bool getIsTrackedNameDifferent() const;
	void setIsTrackedNameDifferent(bool isDifferent);

	std::string getTrackedName() const;
	void setTrackedName(const std::string& trackedName);

	void setShift(uint32_t shift);
	uint32_t getShift() const;

	void setMask(uint32_t mask);
	uint32_t getMask() const;

	void setHighLevelType(HighLevelType type);
	HighLevelType getHighLevelType() const;

	void setFractional(Fractional fractional);
	Variable::Fractional getFractional() const;
	bool isFractional() const;

	void setEnumLabels(const std::vector<EnumLabel>& enumLabels);
	std::vector<EnumLabel> getEnumLabels() const;
	bool isEnum() const;

	/* The name of the value in the enumeration, or an empty string when the
	   variable does not interpret as one or the value is not named. A value that
	   is not named is not an error: the caller falls back to the number. */
	std::string getEnumLabel(double value) const;

	uint32_t getRawFromDouble(double value);
	double transformToDouble();

	/* Range guard for writes to the target. Disabled by default, so the previous
	   behaviour is unchanged until limits are configured explicitly.
	   MCP clients are expected to set them through configure_variable. */
	void setWriteLimits(bool enabled, double min, double max);
	bool getWriteLimitsEnabled() const;
	double getWriteMin() const;
	double getWriteMax() const;
	bool isWriteAllowed(double value) const;

	void setIsCurrentlySampled(bool isCurrentlySampled);
	bool getIsCurrentlySampled() const;

   public:
	static const char* types[8];
	static const char* highLevelTypes[5];

   private:
	std::string name = "";
	std::string trackedName = "";
	Type type = Type::UNKNOWN;
	HighLevelType highLevelType = HighLevelType::NONE;

	double value = 0.0;
	uint32_t rawValue = 0;

	uint32_t address = 0x20000000;
	Fractional fractional{};

	std::vector<EnumLabel> enumLabels{};

	uint32_t shift = 0;
	uint32_t mask = 0xffffffff;

	Color color{};
	bool isFound = false;
	bool shouldUpdateFromElf = true;
	bool isTrackedNameDifferent = false;
	bool isCurrentlySampled = false;

	bool writeLimitsEnabled = false;
	double writeMin = 0.0;
	double writeMax = 0.0;
};

#endif