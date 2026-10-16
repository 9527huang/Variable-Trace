#ifndef _PLOT_HPP
#define _PLOT_HPP

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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
		/*
		 * The variable this series draws.
		 *
		 * A series that comes from the target's memory points at a variable
		 * that was read from it. A derived series - a queue depth, a CPU
		 * load - points at a variable that holds a value worked out on this
		 * side and never read from the target.
		 *
		 * The variable is used either way, and that is the point: cursor
		 * readings, statistics and export all work off it, so a derived
		 * series gets those abilities without a second implementation of any
		 * of them.
		 */
		Variable* var = nullptr;

		/*
		 * True when the value did not come from the target's memory.
		 *
		 * This has to be known here and not guessed from the address: the
		 * sampling loop builds its read plan out of every visible series, and
		 * a derived series has no address that means anything. Letting one
		 * through would put a made up address into that plan, and the loop
		 * would read whatever happens to be there and record it as a value.
		 */
		bool derived = false;

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

	/* Whether the measurements window is open for this plot. It is the row's
	   switch rather than a property of the chosen series, so that the window
	   can be opened before a series is picked - which is the order the two
	   are used in: open it, look at what is on offer, then choose. */
	bool getStatisticsVisible() const { return statisticsVisible; }
	void setStatisticsVisible(bool newVisible) { statisticsVisible = newVisible; }

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
	/*
	 * Adds a series over a variable.
	 *
	 * Whether the series is derived is read off the variable rather than taken
	 * as an argument. Every path that adds a series would otherwise have to
	 * remember to say so, and the one that forgot would put a derived
	 * variable's made up address into the sampling loop's read plan.
	 */
	bool addSeries(Variable* var);
	std::shared_ptr<Plot::Series> getSeries(const std::string& name);
	std::map<std::string, std::shared_ptr<Plot::Series>>& getSeriesMap();

	/* The same map for a caller that only reads it. Copying a plot is such a
	   caller: it walks the series of the plot it copies and has no business
	   changing them. */
	const std::map<std::string, std::shared_ptr<Plot::Series>>& getSeriesMap() const;

	/* Everything that describes the plot rather than the samples that have run
	   through it: which kind of plot it is, what its axes are called, where its
	   cursors and its measurements window sit, and which variables it shows.
	   The samples are left out on purpose - a copy starts collecting from the
	   moment it appears, and carrying the buffer over would make it look like
	   the same run was recorded twice. */
	void copyConfigurationFrom(const Plot& source);
	ScrollingBuffer<double>* getXAxisSeries();
	bool removeSeries(const std::string& name);
	bool removeAllVariables();
	void renameSeries(const std::string& oldName, const std::string newName);
	std::vector<uint32_t> getVariableAddesses() const;
	std::vector<Variable::Type> getVariableTypes() const;
	bool addPoint(const std::string& varName, double value);
	void updateSeries();
	void updateSeries(const std::vector<std::shared_ptr<Series>>& visibleSeries, bool updateXAxis);
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

	/* Whether the view is still carried along by the arriving data.

	   A running plot shows the newest samples and slides that window
	   forward as more of them come in. The moment the reader zooms or pans
	   by hand, that is no longer what they are asking for, so the view is
	   handed over to them and stays where they put it until they ask for
	   the live end back. Nothing about this is saved with a project: it
	   describes what one run is showing, and a project opened tomorrow
	   starts out live. */
	void setViewFollowsLatestData(bool state);
	bool isViewFollowingLatestData() const;

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

	/* The slope between the two corners of a pair of cursors.

	   This is the reading a plot carries when both of its axes are values
	   rather than time: two lines each way mark off a rectangle, and what the
	   rectangle is read for is how much the vertical one changes per unit of
	   the horizontal one. A pair whose corners share a horizontal position
	   has no slope, and says so rather than reporting an infinite number. */
	static std::string formatCursorSlope(double rise, double run);

	/* The stretch of time a live plot shows: a window of the given width
	   whose right edge is the newest sample, and whose left edge is the
	   oldest sample still held whenever the window is wider than the samples
	   are.

	   The width and the samples do not have to agree. The width is worked
	   out from how fast samples are arriving now, and the oldest sample is
	   where the buffer gave up keeping the run's beginning; a window
	   narrower than the samples scrolls, and one wider than them covers all
	   of them and stops at the first. Where it would otherwise start in
	   empty time, the axis would open a gap in front of the trace that grows
	   as more of the beginning is overwritten - which is read as the trace
	   losing its beginning, which it did, but the empty axis is what makes
	   it look like it happened on screen.

	   The two ends are read one after the other from a buffer the sampling
	   thread is writing, so the pair can also arrive the wrong way round; the
	   right edge is the newest sample either way, since that is what the
	   window is anchored on. */
	static std::pair<double, double> liveTimeWindow(double newest, double oldest, double windowWidth);

	/* Whether both cursors of a pair sit at the same place along their axis,
	   in which case the span between them is zero and the readings that
	   divide by it are not numbers. */
	static bool cursorsShareAPosition(double first, double second);

	/* Which way a cursor's reading is pushed, away from the middle of the
	   plot, as the sign of a pixel offset.

	   The middle of a plot is where its data is, so a reading is put on the
	   far side of its line and sits over the margin instead of over the
	   trace. A line left of the middle goes left, one right of it goes
	   right; the same holds below and above.

	   Each line is asked separately rather than the pair once, because the
	   two lines are often on opposite sides of the middle, and then both of
	   their readings belong on the outside - which is the far side for each
	   of them in turn.

	   The two axes get a function each because a pixel offset does not grow
	   the same way along both of them: to the right and downwards. A line in
	   the lower half of a plot moves its reading down, which is a larger y,
	   while the same rule along the bottom moves it to a smaller x. Keeping
	   the two apart is what stops one of them being converted wrongly. */
	static double readingOffsetSignAlongX(double position, double low, double high);

	/* As above, along the vertical axis: -1 pushes the reading up and +1
	   pushes it down. */
	static double readingOffsetSignAlongY(double position, double low, double high);

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
	bool viewFollowsLatestData = true;

	bool cursorsVisible = false;
	CursorMode cursorMode = CursorMode::X;
	bool statisticsVisible = false;
};

#endif
