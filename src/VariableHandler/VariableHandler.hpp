#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <random>

#include "Variable.hpp"

/* The variables and the plots whose series point at them are one model, and two
 * threads want it at once: the interface adds, deletes and renames variables,
 * and the acquisition thread walks the same objects on every sample to find out
 * what to read and where to put it.
 *
 * A plot names its variables through bare pointers, so deleting one is two
 * changes that have to be seen as a single step - the series that point at it
 * go first, while the object is still there, and then the object itself. Split
 * the two and the acquisition thread can walk a series whose pointer has
 * already been freed.
 *
 * The lock below is what makes that step atomic. It is taken by every method
 * here that writes, and by anything that reaches into a plot on their behalf
 * (see removeSeriesCallback), and it is held by the acquisition thread while it
 * reads the model. Lock order, where both are wanted, is this one first and
 * DataHandlerBase::mtx second; nothing takes them the other way round.
 */
class VariableHandler
{
   public:
	using VariableMap = std::map<std::string, std::shared_ptr<Variable>>;

   public:
	/* For the callers that write to a plot rather than to the collection: the
	   one that drops a series the reader just dragged away, and the ones that
	   put a series on a plot. Held for the length of the change. */
	std::unique_lock<std::mutex> lockModel();

	void addVariable(std::shared_ptr<Variable> var);

	std::shared_ptr<Variable> getVariable(const std::string& name);

	void clear();

	bool isEmpty();

	void erase(const std::string& nameToDelete);

	bool contains(const std::string& name);

	void addNewVariable(std::string newName);

	void renameVariable(const std::string& currentName, const std::string& newName);

	class iterator
	{
	   public:
		using iterator_category = std::forward_iterator_tag;
		explicit iterator(std::map<std::string, std::shared_ptr<Variable>>::iterator iter);
		iterator& operator++();
		iterator operator++(int);
		bool operator==(const iterator& other) const;
		bool operator!=(const iterator& other) const;
		std::shared_ptr<Variable> operator*();

	   private:
		std::map<std::string, std::shared_ptr<Variable>>::iterator m_iter;
	};

	iterator begin();
	iterator end();

   public:
	std::function<void(const std::string&, const std::string&)> renameCallback;

	/* Called from erase() with the model lock already held, before the variable
	   is dropped. What it has to do is take the series that point at that
	   variable off the plots, and it must not ask for the lock itself. */
	std::function<void(const std::string&)> removeSeriesCallback;

   private:
	std::mutex modelMutex;
	VariableMap variableMap;
};
