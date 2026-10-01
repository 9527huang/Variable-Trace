#ifndef _PLOT_HPP
#define _PLOT_HPP

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ScrollingBuffer.hpp"
#include "Variable.hpp"

class Plot
{
   public:
	enum class displayFormat
	{
		DEC = 0,
		HEX = 1,
		BIN = 2,
	};
	struct Series
	{
		Variable* var = nullptr;
		displayFormat format = displayFormat::DEC;
		std::unique_ptr<ScrollingBuffer<double>> buffer;
		bool visible = true;

		void addPointFromVar() { buffer->addPoint(var->getValue()); }
	};

	enum class Type : uint8_t
	{
		CURVE = 0,
		BAR = 1,
		TABLE = 2,
		XY = 3
	};

	enum class Domain : uint8_t
	{
		ANALOG = 0,
		DIGITAL = 1,
	};

	enum class TraceVarType : uint8_t
	{
		U8 = 0,
		I8 = 1,
		U16 = 2,
		I16 = 3,
		U32 = 4,
		I32 = 5,
		F32 = 6
	};

	/* Which cursors a plot draws. The two directions are independent: a
	   cursor that measures along the bottom of the plot says nothing about
	   the one that measures up the side, and both may be wanted at once. */
	enum class CursorMode : uint8_t
	{
		X = 0,
		Y = 1,
		XY = 2
	};

	static constexpr const char* cursorModeNames[] = {"X", "Y", "X+Y"};

	/* Reversible so that a project file can hold the number and a combo can
	   show the name without either of them owning the mapping. */
	static CursorMode cursorModeFromIndex(uint32_t index)
	{
		return index <= static_cast<uint32_t>(CursorMode::XY) ? static_cast<CursorMode>(index) : CursorMode::X;
	}

	static uint32_t cursorModeToIndex(CursorMode mode)
	{
		return static_cast<uint32_t>(mode);
	}

	class DragRect
	{
	   public:
		bool getState() const { return state; }
		void setState(bool newState) { state = newState; }

		double getValueX0() const { return std::min(rect.x0, rect.x1); }
		double getValueX1() const { return std::max(rect.x0, rect.x1); }

		void setValueX0(double newX0) { rect.x0 = newX0; }
		void setValueX1(double newX1) { rect.x1 = newX1; }

	   private:
		bool state = false;

		struct Rect
		{
			double x0;
			double x1;
			double y0;
			double y1;
		} rect{};
	};

	class Marker
	{
	   public:
		Marker() : state(false), value(0.0) {}

		bool getState() const { return state; }
		void setState(bool newState) { state = newState; }

		double getValue() const { return value; }
		void setValue(double newValue) { value = newValue; }

	   private:
		bool state;
		double value;
	};

	/* The cursors the plot draws.

	   A pair of X cursors is what makes a measurement possible, so both
	   directions carry two of them: a single line can say where something is,
	   only two of them can say how far apart two things are. Which of them
	   are drawn is read off the mode rather than stored per cursor, so the
	   four lines cannot end up disagreeing about what the user asked for. */
	Marker markerX0{};
	Marker markerX1{};
	Marker markerY0{};
	Marker markerY1{};
	Marker trigger{};

	bool getCursorsVisible() const { return cursorsVisible; }
	void setCursorsVisible(bool newVisible) { cursorsVisible = newVisible; }

	CursorMode getCursorMode() const { return cursorMode; }
	void setCursorMode(CursorMode newMode) { cursorMode = newMode; }

	/* Whether the cursors of each direction are drawn, as a pair with the
	   measurements that go with them. A direction whose cursors are hidden
	   is not drawn at all, so the drawing code asks once instead of testing
	   the mode in every branch. */
	bool drawsXCursors() const { return cursorsVisible && cursorMode != CursorMode::Y; }
	bool drawsYCursors() const { return cursorsVisible && cursorMode != CursorMode::X; }

	DragRect stats{};

	explicit Plot(const std::string& name);
	void setName(const std::string& newName);
	std::string getName() const;
	std::string& getNameVar();
	void setAlias(const std::string& newAlias);
	std::string getAlias() const;
	bool addSeries(Variable* var);
	std::shared_ptr<Plot::Series> getSeries(const std::string& name);
	std::map<std::string, std::shared_ptr<Plot::Series>>& getSeriesMap();
	ScrollingBuffer<double>* getXAxisSeries();
	bool removeSeries(const std::string& name);
	bool removeAllVariables();
	void renameSeries(const std::string& oldName, const std::string newName);
	std::vector<uint32_t> getVariableAddesses() const;
	std::vector<Variable::Type> getVariableTypes() const;
	bool addPoint(const std::string& varName, double value);
	void updateSeries();
	bool addTimePoint(double t);
	void erase();
	void setVisibility(bool state);
	bool getVisibility() const;
	bool& getVisibilityVar();

	void setType(Type newType);
	Type getType() const;

	/* TODO: Domain and TraceVarType should be in a derived class only */
	void setDomain(Domain newDomain);
	Domain getDomain() const;

	void setTraceVarType(TraceVarType newTraceVarType);
	TraceVarType getTraceVarType() const;

	void setIsHovered(bool isHovered);
	bool isHovered() const;

	Variable* getXAxisVariable() const;
	void setXAxisVariable(Variable* var);

	/* Text drawn along each axis. An empty string is the normal state and
	   means the axis keeps whatever it was called before a label could be
	   typed in, so an older project reads back looking the same. */
	void setXAxisLabel(const std::string& newLabel);
	void setYAxisLabel(const std::string& newLabel);
	std::string getXAxisLabel() const;
	std::string getYAxisLabel() const;

	/* What the axis is called with no label set. A curve is drawn against
	   time and an XY plot against another variable, which is where the two
	   horizontal names come from; the vertical axis holds the values of the
	   plotted variables and is named after that. Only a table, which has no
	   axes, leaves a name empty. */
	std::string getDefaultXAxisLabel() const;
	std::string getDefaultYAxisLabel() const;

	/* The text to draw: the label when one was typed in, the default
	   otherwise. Both drawing and the editor read this, so the field in the
	   editor always describes what is on screen. */
	std::string getEffectiveXAxisLabel() const;
	std::string getEffectiveYAxisLabel() const;

	displayFormat getSeriesDisplayFormat(const std::string& name) const;
	void setSeriesDisplayFormat(const std::string& name, displayFormat format);
	std::string getSeriesValueString(const std::string& name, double value);

	/* How a cursor reading is written out.

	   These live here rather than with the drawing code so that they can be
	   read and checked without a window: they are text formatting and nothing
	   else. A cursor sitting on a whole number should not read as that number
	   plus five zeroes, so a fixed point form is only used while there is
	   something after the point worth showing. */
	static std::string formatCursorValue(double value);

	/* A time in milliseconds. The horizontal axis of a curve is drawn in
	   seconds, but the spans a cursor measures are the ones between one run
	   of the firmware and the next, and those are milliseconds. The unit is
	   spelled out because a reading of 0.366667 makes the reader convert it
	   and 366.667 ms does not. */
	static std::string formatCursorMilliseconds(double seconds);

	/* The rate a span stands for, in hertz. A pair of time cursors measures a
	   period, and the useful reading of a period is usually how often it
	   repeats. A pair sitting on one instant has no rate, and says so rather
	   than reporting an infinite number. */
	static std::string formatCursorRate(double seconds);

	int32_t statisticsSeries = 0;

   private:
	std::string name;
	std::string alias;
	std::string xAxisLabel;
	std::string yAxisLabel;
	std::map<std::string, std::shared_ptr<Series>> seriesMap;
	ScrollingBuffer<double> time;
	Series xAxisSeries;
	bool visibility = true;
	Type type = Type::CURVE;
	Domain domain = Domain::ANALOG;
	TraceVarType traceVarType = TraceVarType::F32;
	bool isHoveredOver = false;

	bool cursorsVisible = false;
	CursorMode cursorMode = CursorMode::X;
};

#endif