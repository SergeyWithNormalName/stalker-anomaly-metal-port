////////////////////////////////////////////////////////////////////////////
//	Module 		: graph_edge_inline.h
//	Created 	: 14.01.2004
//  Modified 	: 19.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Graph edge class template inline functions
////////////////////////////////////////////////////////////////////////////

#pragma once

#define TEMPLATE_SPECIALIZATION template <\
	typename EdgeWeightType,\
	typename VertexType\
>

#define CSGraphEdge CEdgeBase<EdgeWeightType, VertexType>

TEMPLATE_SPECIALIZATION
IC CSGraphEdge::CEdgeBase(const EdgeWeightType& weight, VertexType* vertex)
{
	m_weight = weight;
	VERIFY(vertex);
	m_vertex = vertex;
}

TEMPLATE_SPECIALIZATION
IC const EdgeWeightType& CSGraphEdge::weight() const
{
	return (m_weight);
}

TEMPLATE_SPECIALIZATION
IC VertexType* CSGraphEdge::vertex() const
{
	return (m_vertex);
}

TEMPLATE_SPECIALIZATION
IC const typename CSGraphEdge::_vertex_id_type& CSGraphEdge::vertex_id() const
{
	return (vertex()->vertex_id());
}

#undef TEMPLATE_SPECIALIZATION
#undef CSGraphEdge

////////////////////////////////////////////////////////////////////////////
// class CEdge
////////////////////////////////////////////////////////////////////////////

#define TEMPLATE_SPECIALIZATION template <\
	typename EdgeWeightType,\
	typename VertexType,\
	typename EdgeDataType\
>

#define CSGraphEdge CEdge<EdgeWeightType, VertexType, EdgeDataType>

TEMPLATE_SPECIALIZATION
IC CSGraphEdge::CEdge(const EdgeWeightType& weight, VertexType* vertex) :
	inherited(weight, vertex)
{
}

TEMPLATE_SPECIALIZATION
IC bool CSGraphEdge::operator==(const _vertex_id_type& vertex_id) const
{
	return (this->vertex()->vertex_id() == vertex_id);
}

TEMPLATE_SPECIALIZATION
IC bool CSGraphEdge::operator==(const CEdge& obj) const
{
	if (this->weight() != obj.weight())
		return (false);

	return (this->vertex()->vertex_id() == obj.vertex()->vertex_id());
}

TEMPLATE_SPECIALIZATION
IC const EdgeDataType& CSGraphEdge::data() const
{
	return (m_data);
}

TEMPLATE_SPECIALIZATION
IC EdgeDataType& CSGraphEdge::data()
{
	return (m_data);
}

#undef TEMPLATE_SPECIALIZATION
#undef CSGraphEdge

////////////////////////////////////////////////////////////////////////////
// class CEdge<..., xr_empty>
////////////////////////////////////////////////////////////////////////////

#define TEMPLATE_SPECIALIZATION template <\
	typename EdgeWeightType,\
	typename VertexType\
>

#define CSGraphEdge CEdge<EdgeWeightType, VertexType, xr_empty>

TEMPLATE_SPECIALIZATION
IC CSGraphEdge::CEdge(const EdgeWeightType& weight, VertexType* vertex) :
	inherited(weight, vertex)
{
}

TEMPLATE_SPECIALIZATION
IC bool CSGraphEdge::operator==(const _vertex_id_type& vertex_id) const
{
	return (this->vertex()->vertex_id() == vertex_id);
}

TEMPLATE_SPECIALIZATION
IC bool CSGraphEdge::operator==(const CEdge& obj) const
{
	if (this->weight() != obj.weight())
		return (false);

	return (this->vertex()->vertex_id() == obj.vertex()->vertex_id());
}

#undef TEMPLATE_SPECIALIZATION
#undef CSGraphEdge
