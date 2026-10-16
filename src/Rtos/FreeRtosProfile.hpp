#ifndef _FREERTOSPROFILE_HPP
#define _FREERTOSPROFILE_HPP

#include "RtosProfileBase.hpp"

/*
 * FreeRTOS, as a table.
 *
 * The symbol names come from the kernel's own globals. They are file scope
 * statics, so a symbol file with its symbols trimmed loses them while the
 * debug information keeps them; the reader has to prefer debug information for
 * that reason, not because the names are unusual.
 *
 * `tskKERNEL_VERSION_NUMBER` is deliberately absent. It is a macro, not a
 * symbol: it survives into the object file only as a string literal, and only
 * into `.debug_macro` when the build carried macro debug information (GCC's
 * `-g3`). The version is reported when it can be found and called unknown when
 * it cannot, and nothing downstream may branch on it.
 */
class FreeRtosProfile : public RtosProfileBase
{
   public:
	Type getType() const override;
	std::string getDescription() const override;

	const std::vector<SymbolSpec>& getSymbols() const override;
	const std::vector<FieldSpec>& getFields() const override;

	/* FreeRTOS numbers priorities upwards: the largest number runs first. */
	bool higherNumberIsHigherPriority() const override;

	/* The default of `configMAX_TASK_NAME_LEN` in the kernel. A project that
	   changes it gets a truncated or padded name, which the task table shows
	   for what it is rather than hiding. */
	uint32_t taskNameCapacity() const override;

	/*
	 * Offsets measured on FreeRTOS V11.1.0 built by arm-none-eabi-gcc.
	 *
	 * These are a last resort for a target whose debug information cannot
	 * answer, and every entry in the table is marked as a fallback so that the
	 * interface never presents them as readings. They are only correct for a
	 * build that left the list integrity checking options off, which is why
	 * the marked origin travels with them.
	 */
	RtosLayoutTable getFallbackLayout() const override;

	/* Names the versions of the kernel this table describes. Reported next to
	   the detection result, never used to decide anything. */
	static const char* testedKernelVersion();
};

#endif
