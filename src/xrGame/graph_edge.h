////////////////////////////////////////////////////////////////////////////
//	Module 		: CEdge.h
//	Created 	: 14.01.2004
//  Modified 	: 19.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Graph edge class template
////////////////////////////////////////////////////////////////////////////

#pragma once

template <typename EdgeWeightType, typename VertexType>
class CEdgeBase
{
public:
	typedef EdgeWeightType _edge_weight_type;
	typedef VertexType _vertex_type;
	typedef typename VertexType::_vertex_id_type _vertex_id_type;

private:
	EdgeWeightType m_weight;
	VertexType* m_vertex;

public:
	IC CEdgeBase(const EdgeWeightType& weight, VertexType* vertex);
	IC const EdgeWeightType& weight() const;
	IC VertexType* vertex() const;
	IC const _vertex_id_type& vertex_id() const;
};

template <typename EdgeWeightType, typename VertexType, typename EdgeDataType>
class CEdge : 
	public CEdgeBase<EdgeWeightType, VertexType> 
{
private:
	typedef CEdgeBase<EdgeWeightType, VertexType> inherited;

public:
	using _vertex_id_type = typename inherited::_vertex_id_type;
	using _edge_weight_type = EdgeWeightType;
	using _vertex_type = VertexType;
	using _edge_data_type = EdgeDataType;

private:
	EdgeDataType m_data;

public:
	IC CEdge(const EdgeWeightType& weight, VertexType* vertex);
	IC bool operator==(const _vertex_id_type& vertex_id) const;
	IC bool operator==(const CEdge& obj) const;
	IC const EdgeDataType& data() const;
	IC EdgeDataType& data();
};

template <typename EdgeWeightType, typename VertexType>
class CEdge<EdgeWeightType, VertexType, xr_empty> : 
	public CEdgeBase<EdgeWeightType, VertexType> {
private:
	typedef CEdgeBase<EdgeWeightType, VertexType> inherited;

public:
	using _vertex_id_type = typename inherited::_vertex_id_type;
	using _edge_weight_type = EdgeWeightType;
	using _vertex_type = VertexType;

public:
	IC CEdge(const EdgeWeightType& weight, VertexType* vertex);
	IC bool operator==(const _vertex_id_type& vertex_id) const;
	IC bool operator==(const CEdge& obj) const;
};

#include "graph_edge_inline.h"
