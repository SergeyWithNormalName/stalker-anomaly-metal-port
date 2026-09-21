////////////////////////////////////////////////////////////////////////////
//	Module 		: operator_condition.h
//	Created 	: 24.02.2004
//  Modified 	: 24.02.2004
//	Author		: Dmitriy Iassenev
//	Description : Operator condition
////////////////////////////////////////////////////////////////////////////

#pragma once

template <
	typename ConditionType,
	typename ValueType
>
class COperatorConditionAbstract
{
public:
	typedef ConditionType _condition_type;
	typedef ValueType _value_type;

protected:
	typedef COperatorConditionAbstract<ConditionType, ValueType> COperatorCondition;

protected:
	ConditionType m_condition;
	u32 m_hash;
	ValueType m_value;

public:
	IC COperatorConditionAbstract(const ConditionType condition, const ValueType value);
	IC const ConditionType& condition() const;
	IC const ValueType& value() const;
	IC const u32& hash_value() const;
	IC bool operator<(const COperatorCondition& condition) const;
	IC bool operator==(const COperatorCondition& condition) const;
};

#include "operator_condition_inline.h"
