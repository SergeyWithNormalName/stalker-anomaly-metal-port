////////////////////////////////////////////////////////////////////////////
//	Module 		: graph_abstract.h
//	Created 	: 14.01.2004
//  Modified 	: 19.02.2005
//	Author		: Dmitriy Iassenev
//	Description : Graph class template
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "graph_vertex.h"
#include "graph_edge.h"
#include "object_broker.h"

template <
	typename DataType = xr_empty,
	typename EdgeWeightType = float,
	typename VertexIdType = u32,
	typename EdgeDataType = xr_empty
>
class CGraphAbstract
{
public:
	typedef CVertex<
		DataType,
		VertexIdType,
		CGraphAbstract
	> CVertex;

	typedef CEdge<
		EdgeWeightType,
		CVertex,
		EdgeDataType
	> CEdge;

public:
	typedef xr_map<VertexIdType, CVertex*> VERTICES;
	typedef typename CVertex::EDGES EDGES;

public:
	typedef typename VERTICES::const_iterator const_vertex_iterator;
	typedef typename VERTICES::iterator vertex_iterator;
	typedef typename EDGES::const_iterator const_iterator;
	typedef typename EDGES::iterator iterator;
	typedef VertexIdType _vertex_id_type;
	typedef EdgeWeightType _edge_weight_type;

private:
	VERTICES m_vertices;
	size_t m_edge_count;

public:
	IC CGraphAbstract();
	virtual ~CGraphAbstract();
	IC bool operator==(const CGraphAbstract& obj) const;
	IC void clear();
	IC void add_vertex(const DataType& data, const VertexIdType& vertex_id);
	IC void remove_vertex(const VertexIdType& vertex_id);
	IC void add_edge(const VertexIdType& vertex_id0, const VertexIdType& vertex_id1,
	                 const EdgeWeightType& edge_weight);
	IC void add_edge(const VertexIdType& vertex_id0, const VertexIdType& vertex_id1,
	                 const EdgeWeightType& edge_weight0, const EdgeWeightType& edge_weight1);
	IC void remove_edge(const VertexIdType& vertex_id0, const VertexIdType& vertex_id1);
	IC u32 vertex_count() const;
	IC size_t edge_count() const;
	IC bool empty() const;
	IC const VERTICES& vertices() const;
	IC VERTICES& vertices();
	IC const CVertex* vertex(const VertexIdType& vertex_id) const;
	IC CVertex* vertex(const VertexIdType& vertex_id);
	IC const CEdge* edge(const VertexIdType& vertex_id0, const VertexIdType& vertex_id1) const;
	IC CEdge* edge(const VertexIdType& vertex_id0, const VertexIdType& vertex_id1);
	IC const CGraphAbstract& header() const;
	IC const EdgeWeightType get_edge_weight(const VertexIdType vertex_index0, const VertexIdType vertex_index1,
	                                           const_iterator i) const;
	IC bool is_accessible(const VertexIdType vertex_index) const;
	IC VertexIdType const& value(VertexIdType const& vertex_index, const_iterator i) const;
	IC void begin(const CVertex* vertex, const_iterator& b, const_iterator& e) const;
	IC void begin(VertexIdType const& vertex_index, const_iterator& b, const_iterator& e) const;
};

template <
	typename DataType = xr_empty,
	typename EdgeWeightType = float,
	typename VertexIdType = u32
>
class CGraphAbstractSerialize : public CGraphAbstract<DataType, EdgeWeightType, VertexIdType>,
                                public IPureSerializeObject<IReader, IWriter>
{
public:
	virtual void save(IWriter& stream);
	virtual void load(IReader& stream);
};

#include "graph_abstract_inline.h"
