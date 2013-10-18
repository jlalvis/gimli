/***************************************************************************
 *   Copyright (C) 2006-2013 by the resistivity.net development team       *
 *   Carsten Rücker carsten@resistivity.net                                *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "mesh.h"

#include "kdtreeWrapper.h"
#include "meshentities.h"
#include "node.h"
#include "shape.h"
#include "sparsematrix.h"
#include "stopwatch.h"

#include <boost/bind.hpp>

#include <map>

namespace GIMLI{

std::ostream & operator << (std::ostream & str, const Mesh & mesh){
    str << "\tNodes: " << mesh.nodeCount() << "\tCells: " << mesh.cellCount() << "\tBoundaries: " << mesh.boundaryCount();
    return str;
}

Mesh::Mesh(uint dim)
: dimension_(dim), rangesKnown_(false), neighboursKnown_(false), tree_(NULL){
    oldTet10NumberingStyle_ = true;
}

Mesh::Mesh(const std::string & filename)
: rangesKnown_(false), neighboursKnown_(false), tree_(NULL){
    dimension_ = 3;
    oldTet10NumberingStyle_ = true;
    load(filename);
}

Mesh::Mesh(const Mesh & mesh)
: rangesKnown_(false), neighboursKnown_(false), tree_(NULL){
    oldTet10NumberingStyle_ = true;
    copy_(mesh);
}

Mesh & Mesh::operator = (const Mesh & mesh){
    if (this != & mesh){
        copy_(mesh);
    } return *this;
}

void Mesh::copy_(const Mesh & mesh){
    clear();
    rangesKnown_ = false;
    dimension_ = mesh.dim();
    nodeVector_.reserve(mesh.nodeCount());
    for (uint i = 0; i < mesh.nodeCount(); i ++) createNode(mesh.node(i));

    boundaryVector_.reserve(mesh.boundaryCount());
    for (uint i = 0; i < mesh.boundaryCount(); i ++) createBoundary(mesh.boundary(i));

    cellVector_.reserve(mesh.cellCount());
    for (uint i = 0; i < mesh.cellCount(); i ++) createCell(mesh.cell(i));

    setExportDataMap(mesh.exportDataMap());
    setCellAttributes(mesh.cellAttributes());

    if (mesh.neighboursKnown()){
        this->createNeighbourInfos(true);
    }
//     std::cout << "COPY mesh " << mesh.cell(0) << " " << cell(0) << std::endl;
}

Mesh::~Mesh(){
    clear();
}

void Mesh::clear(){
    if (tree_) {
        deletePtr()(tree_);
        tree_ = NULL;
    }

    for_each(cellVector_.begin(), cellVector_.end(), deletePtr());
    cellVector_.clear();

    for_each(boundaryVector_.begin(), boundaryVector_.end(), deletePtr());
    boundaryVector_.clear();

    for_each(nodeVector_.begin(), nodeVector_.end(), deletePtr());
    nodeVector_.clear();


    rangesKnown_ = false;
    neighboursKnown_ = false;
}

Node * Mesh::createNode_(const RVector3 & pos, int marker, int id){
    if (id == -1) id = nodeCount();
    nodeVector_.push_back(new Node(pos));
    nodeVector_.back()->setMarker(marker);
    nodeVector_.back()->setId(id);
    return nodeVector_.back();
}

Node * Mesh::createNode(const Node & node){
    return createNode_(node.pos(), node.marker(), -1);
}

Node * Mesh::createNode(double x, double y, double z, int marker){
    return createNode_(RVector3(x, y, z), marker, -1);
}

Node * Mesh::createNode(const RVector3 & pos, int marker){
    return createNode_(pos, marker, -1);
}

Node * Mesh::createNodeWithCheck(const RVector3 & pos, double tol){
    fillKDTree_();

    Node * refNode = tree_->nearest(pos);
    if (refNode){
        if (pos.distance(refNode->pos()) < tol) return refNode;
    }

//     for (uint i = 0, imax = nodeVector_.size(); i < imax; i++){
//         if (pos.distance(nodeVector_[i]->pos()) < 1e-6) return nodeVector_[i];
//     }

    Node * newNode = createNode(pos);
    tree_->insert(newNode);
    return newNode;
}

void Mesh::findRange_() const{
    if (!rangesKnown_){
        minRange_ = RVector3(MAX_DOUBLE, MAX_DOUBLE, MAX_DOUBLE);
        maxRange_ = -minRange_;
        for (uint i = 0; i < nodeVector_.size(); i ++){
            for (uint j = 0; j < 3; j ++){
                minRange_[j] = min(nodeVector_[i]->pos()[j], minRange_[j]);
                maxRange_[j] = max(nodeVector_[i]->pos()[j], maxRange_[j]);
            }
        }
        rangesKnown_ = true;
    }
}

Boundary * Mesh::createBoundary(std::vector < Node * > & nodes, int marker){
    switch (nodes.size()){
      case 1: return createBoundaryChecked_< NodeBoundary >(nodes, marker); break;
      case 2: return createBoundaryChecked_< Edge >(nodes, marker); break;
      case 3: {
        if (dimension_ == 2)
            return createBoundaryChecked_< Edge3 >(nodes, marker);
        return createBoundaryChecked_< TriangleFace >(nodes, marker); } break;
      case 4: return createBoundaryChecked_< QuadrangleFace >(nodes, marker); break;
      case 6: return createBoundaryChecked_< Triangle6Face >(nodes, marker); break;
      case 8: return createBoundaryChecked_< Quadrangle8Face >(nodes, marker); break;
    }
    std::cout << WHERE_AM_I << "WHERE_AM_I << cannot determine boundary for nodes: " << nodes.size() << std::endl;
    return NULL;
}

Boundary * Mesh::createBoundary(const Boundary & bound){
    std::vector < Node * > nodes(bound.nodeCount());
    for (uint i = 0; i < bound.nodeCount(); i ++) nodes[i] = &node(bound.node(i).id());
    return createBoundary(nodes, bound.marker());
}

Boundary * Mesh::createBoundary(const Cell & cell){
    std::vector < Node * > nodes(cell.nodeCount());
    for (uint i = 0; i < cell.nodeCount(); i ++) nodes[i] = &node(cell.node(i).id());
    return createBoundary(nodes, cell.marker());
}

Boundary * Mesh::createNodeBoundary(Node & n1, int marker){
    std::vector < Node * > nodes(1); nodes[0] = & n1;
    return createBoundaryChecked_< NodeBoundary >(nodes, marker);
}

Boundary * Mesh::createEdge(Node & n1, Node & n2, int marker){
    std::vector < Node * > nodes(2);  nodes[0] = & n1; nodes[1] = & n2;
    return createBoundaryChecked_< Edge >(nodes, marker);
}

Boundary * Mesh::createEdge3(Node & n1, Node & n2, Node & n3, int marker){
    std::vector < Node * > nodes(3); nodes[0] = & n1; nodes[1] = & n2; nodes[2] = & n3;
    return createBoundaryChecked_< Edge3 >(nodes, marker);
}

Boundary * Mesh::createTriangleFace(Node & n1, Node & n2, Node & n3, int marker){
    std::vector < Node * > nodes(3);  nodes[0] = & n1; nodes[1] = & n2; nodes[2] = & n3;
    return createBoundaryChecked_< TriangleFace >(nodes, marker);
}

Boundary * Mesh::createQuadrangleFace(Node & n1, Node & n2, Node & n3, Node & n4, int marker){
    std::vector < Node * > nodes(4);  nodes[0] = & n1; nodes[1] = & n2; nodes[2] = & n3, nodes[3] = & n4;
    return createBoundaryChecked_< QuadrangleFace >(nodes, marker);
}

Cell * Mesh::createCell(int marker){
    std::vector < Node * > nodes(0);
    return createCell_< Cell >(nodes, marker, cellCount());
}

Cell * Mesh::createCell(std::vector < Node * > & nodes, int marker){
    switch (nodes.size()){
        case 0: return createCell_< Cell >(nodes, marker, cellCount()); break;
        case 2: return createCell_< EdgeCell >(nodes, marker, cellCount()); break;
        case 3:
            switch (dimension_){
                case 1: return createCell_< Edge3Cell >(nodes, marker, cellCount()); break;
                case 2: return createCell_< Triangle >(nodes, marker, cellCount()); break;
            }
            break;
        case 4:
            switch (dimension_){
                case 2: return createCell_< Quadrangle >(nodes, marker, cellCount()); break;
                case 3: return createCell_< Tetrahedron >(nodes, marker, cellCount()); break;
            }
            break;
        case 5: return createCell_< Pyramid >(nodes, marker, cellCount()); break;
        case 6:
            switch (dimension_){
                case 2: return createCell_< Triangle6 >(nodes, marker, cellCount()); break;
                case 3: return createCell_< TriPrism >(nodes, marker, cellCount()); break;
            }
            break;
        case 8:
            switch (dimension_){
                case 2: return createCell_< Quadrangle8 >(nodes, marker, cellCount()); break;
                case 3: return createCell_< Hexahedron >(nodes, marker, cellCount()); break;
            }
            break;
        case 10: return createCell_< Tetrahedron10 >(nodes, marker, cellCount()); break;
        case 13: return createCell_< Pyramid13 >(nodes, marker, cellCount()); break;
        case 15: return createCell_< TriPrism15 >(nodes, marker, cellCount()); break;
        case 20: return createCell_< Hexahedron20 >(nodes, marker, cellCount()); break;

    }
    std::cout << WHERE_AM_I << "WHERE_AM_I << cannot determine cell for nodes: " << nodes.size() << " for dim: " << dimension_ << std::endl;
    return NULL;
}

Cell * Mesh::createCell(const Cell & cell){
    std::vector < Node * > nodes(cell.nodeCount());
    for (uint i = 0; i < cell.nodeCount(); i ++) nodes[i] = &node(cell.node(i).id());
    return createCell(nodes, cell.marker());
}

Cell * Mesh::createTriangle(Node & n1, Node & n2, Node & n3, int marker){
    std::vector < Node * > nodes(3);  nodes[0] = & n1; nodes[1] = & n2; nodes[2] = & n3;
    return createCell_< Triangle >(nodes, marker, cellCount());
}

Cell * Mesh::createQuadrangle(Node & n1, Node & n2, Node & n3, Node & n4, int marker){
    std::vector < Node * > nodes(4);
    nodes[0] = & n1; nodes[1] = & n2; nodes[2] = & n3; nodes[3] = & n4;
    return createCell_< Quadrangle >(nodes, marker, cellCount());
}

Cell * Mesh::createTetrahedron(Node & n1, Node & n2, Node & n3, Node & n4, int marker){
    std::vector < Node * > nodes(4);
    nodes[0] = & n1; nodes[1] = & n2; nodes[2] = & n3; nodes[3] = & n4;
    return createCell_< Tetrahedron >(nodes, marker, cellCount());
}

Cell * Mesh::copyCell(const Cell & cell){
    std::vector < Node * > nodes(cell.nodeCount());
    for (uint i = 0; i < nodes.size(); i ++) {
        nodes[i] = createNodeWithCheck(cell.node(i).pos());
        nodes[i]->setMarker(cell.node(i).marker());
    }
    Cell * c = createCell(nodes);

    c->setMarker(cell.marker());
    c->setAttribute(cell.attribute());
    return c;
}

Boundary * Mesh::copyBoundary(const Boundary & bound){
    std::vector < Node * > nodes(bound.nodeCount());
    for (uint i = 0; i < nodes.size(); i ++) {
        nodes[i] = createNodeWithCheck(bound.node(i).pos());
        nodes[i]->setMarker(bound.node(i).marker());
    }
    Boundary * b = createBoundary(nodes);

    b->setMarker(bound.marker());
    return b;
}

Node & Mesh::node(uint i) {
    if (i < 0 || i > nodeCount() - 1){
        std::cerr << WHERE_AM_I << " requested node: " << i << " does not exist." << std::endl;
        exit(EXIT_MESH_NO_NODE);
    } return *nodeVector_[i];
}

Node & Mesh::node(uint i) const {
    if (i < 0 || i > nodeCount() - 1){
        std::cerr << WHERE_AM_I << " requested node: " << i << " does not exist." << std::endl;
        exit(EXIT_MESH_NO_NODE);
    } return *nodeVector_[i];
}

Cell & Mesh::cell(uint i) const {
    if (i < 0 || i > cellCount() - 1){
      std::cerr << WHERE_AM_I << " requested cell: " << i << " does not exist." << std::endl;
      exit(EXIT_MESH_NO_CELL);
    } return *cellVector_[i];
}

Cell & Mesh::cell(uint i) {
    if (i < 0 || i > cellCount() - 1){
      std::cerr << WHERE_AM_I << " requested cell: " << i << " does not exist." << std::endl;
      exit(EXIT_MESH_NO_CELL);
    } return *cellVector_[i];
}

Boundary & Mesh::boundary(uint i) const {
    if (i < 0 || i > boundaryCount() - 1){
      std::cerr << WHERE_AM_I << " requested boundary: " << i << " does not exist." << std::endl;
      exit(EXIT_MESH_NO_BOUNDARY);
    } return *boundaryVector_[i];
}

Boundary & Mesh::boundary(uint i) {
    if (i < 0 || i > boundaryCount() - 1){
      std::cerr << WHERE_AM_I << " requested boundary: " << i << " does not exist." << std::endl;
      exit(EXIT_MESH_NO_BOUNDARY);
    } return *boundaryVector_[i];
}

void Mesh::createHull(const Mesh & mesh){
    if (dim() == 3 && mesh.dim() == 2){
        clear();
        rangesKnown_ = false;
        nodeVector_.reserve(mesh.nodeCount());
        for (uint i = 0; i < mesh.nodeCount(); i ++) createNode(mesh.node(i));

        boundaryVector_.reserve(mesh.cellCount());
        for (uint i = 0; i < mesh.cellCount(); i ++) createBoundary(mesh.cell(i));
    } else {
        std::cerr << WHERE_AM_I << " increasing dimension fails, you should set the dimension for this mesh to 3" << std::endl;
    }
}

uint Mesh::findNearestNode(const RVector3 & pos){
    fillKDTree_();
    return tree_->nearest(pos)->id();
}

std::vector < int > cellIDX__;

Cell * Mesh::findCellBySlopeSearch_(const RVector3 & pos, Cell * start, 
                                    size_t & count, bool tagging) const {
    Cell * cell = start;
    
    do {
        if (cell->tagged() && tagging) {
            cell = NULL;
        } else {
            cell->tag();
            cellIDX__.push_back(cell->id());
            RVector sf;

//             std::cout << "testpos: " << pos << std::endl;
//             std::cout << "cell: " << *cell << " touch: " << cell->shape().isInside(pos, true) << std::endl;
//             for (uint i = 0; i < cell->nodeCount() ; i ++){
//                 std::cout << cell->node(i)<< std::endl;
//             }

            if (cell->shape().isInside(pos, sf, false)) {
                return cell;
            } else {
                
                if (!neighboursKnown_){
                    const_cast<Mesh*>(this)->createNeighbourInfosCell_(cell);
//                     for (uint j = 0; j < cell->neighbourCellCount(); j++){
//                          cell->findNeighbourCell(j);
//                     }
                }
                cell = cell->neighbourCell(sf);

//                  std::cout << "sf: " << sf << std::endl;
//                  std::cout << "neighCell " << cell << std::endl;
            }
            count++;
//             if (count == 20){
//                 std::cout << "testpos: " << pos << std::endl;
//                 std::cout << "cell: " << this->cell(cellIDX__.back()) << std::endl;
//                 for (uint i = 0; i < this->cell(cellIDX__.back()).nodeCount() ; i ++){
//                     std::cout << this->cell(cellIDX__.back()).node(i)<< std::endl;
//                 }
//
//                 std::cout << WHERE_AM_I << " exit with submesh " << std::endl;
//                 std::cout << "probably cant find a cell for " << pos << std::endl;
//                 Mesh subMesh; subMesh.createMeshByCellIdx(*this, cellIDX__);
//
//                 subMesh.exportVTK("submesh");
//                 this->cell(2368).setAttribute(3);
//                 this->exportVTK("submeshParent");
//
//                 exit(0);
//             }
        }
    } while (cell);

    return NULL;
}

Cell * Mesh::findCell(const RVector3 & pos, size_t & count, bool extensive) const {
    bool bruteForce = false;
    Cell * cell = NULL;

    if (bruteForce){
        for (uint i = 0; i < this->cellCount(); i ++) {
            count++;
            if (cellVector_[i]->shape().isInside(pos)){
                cell = cellVector_[i];
                break;
            }
        }
    } else {
        Stopwatch swatch(true);
        cellIDX__.clear();
        count = 0;
        fillKDTree_();
        Node * refNode = tree_->nearest(pos);
        if (!refNode){
            std::cout << "pos: " << pos << std::endl;
            throwError(1, WHERE_AM_I + " no nearest node to pos. This is a empty mesh");
        }
        if (refNode->cellSet().empty()){
            std::cout << "Node: " << *refNode << std::endl;
            throwError(1, WHERE_AM_I + " no cells for this node. This is a corrupt mesh");
        }
            //std::cout << "Node: " << *refNode << std::endl;
//         for (std::set< Cell * >::iterator it = refNode->cellSet().begin(); it != refNode->cellSet().end(); it ++){
//             std::cout << (*it)->id() << std::endl;
//         }
        cell = findCellBySlopeSearch_(pos, *refNode->cellSet().begin(), count, false);
        if (cell) return cell;

//         std::cout << "more expensive test here" << std::endl;
//         exportVTK("slopesearch");
//         exit(0);
        if (extensive){
            std::for_each(cellVector_.begin(), cellVector_.end(), std::mem_fun(&Cell::untag));
            //!** *sigh, no luck with simple kd-tree search, try more expensive full slope search
            count = 0;
            for (uint i = 0; i < this->cellCount(); i ++) {
                cell = cellVector_[i];
                cell = findCellBySlopeSearch_(pos, cell, count, true);
                if (cell) break;
            }
        } else {
            return NULL;
        }
  //      std::cout << " n: " << count;
    }
    return cell;
}

std::vector < Boundary * > Mesh::findBoundaryByMarker(int marker) const {
    return findBoundaryByMarker(marker, marker + 1);
}

std::vector < Boundary * > Mesh::findBoundaryByMarker(int from, int to) const {
    std::vector < Boundary * > vBounds;
    vBounds.reserve(boundaryCount());

    for(std::vector< Boundary * >::const_iterator it = boundaryVector_.begin();
                                               it != boundaryVector_.end(); it++){
        if ((*it)->marker() >= from && (*it)->marker() < to) vBounds.push_back((*it));
    }

    return vBounds;
}

std::vector < Cell * > Mesh::findCellByMarker(int from, int to) const {
    if (to == -1) to = MAX_INT;
    else if (to == 0) to = from + 1;

    std::vector < Cell * > vCell;
    vCell.reserve(cellCount());
    for(std::vector< Cell * >::const_iterator it = cellVector_.begin();
                                               it != cellVector_.end(); it++){
        if ((*it)->marker() >= from && (*it)->marker() < to) vCell.push_back((*it));
    }
    return vCell;
}

std::vector < Cell * > Mesh::findCellByAttribute(double from, double to) const {
    std::vector < Cell * > vCell;
    vCell.reserve(cellCount());

    if (to < TOLERANCE){
        for (uint i = 0; i < cellCount(); i ++){
            if ((cell(i).attribute() - from) < TOLERANCE) vCell.push_back(cellVector_[i]);
        }
    } else {
        if (to == -1) to = MAX_DOUBLE;
        for (uint i = 0; i < cellCount(); i ++){
            if (cell(i).attribute() >= from && cell(i).attribute() < to)
                vCell.push_back(cellVector_[i]);
        }
    }
    return vCell;
}

IndexArray Mesh::findNodesIdxByMarker(int marker) const {
    return find(this->nodeMarker() == marker);
//     std::vector < uint > idx; idx.reserve(nodeCount());
//     for (uint i = 0; i < nodeCount(); i ++) {
//         if (node(i).marker() == marker) idx.push_back(i);
//     }
//     return idx;
}

// std::list < uint > Mesh::findListNodesIdxByMarker(int marker) const {
//     std::list < uint > idx;
//     for (uint i = 0; i < nodeCount(); i ++) {
//         if (node(i).marker() == marker) idx.push_back(i);
//     }
//     return idx;
// }

std::vector < RVector3 > Mesh::positions() const {
    IndexArray idx(this->nodeCount());
    std::generate(idx.begin(), idx.end(), IncrementSequence< uint >(0));
    return this->positions(idx);
}

std::vector < RVector3 > Mesh::positions(const IndexArray & idx) const {
    std::vector < RVector3 > pos; pos.reserve(idx.size());
    for (uint i = 0; i < idx.size(); i ++) {
        pos.push_back(node(idx[i]).pos());
    }
    return pos;
}

std::vector < RVector3 > Mesh::cellCenters() const {
    std::vector < RVector3 > pos; pos.reserve(this->cellCount());
    for (uint i = 0; i < this->cellCount(); i ++) {
        pos.push_back(cellVector_[i]->center());
    }
    return pos;
}

RVector Mesh::cellSizes() const{
    RVector tmp(cellCount());
//     std::transform(cellVector_.begin(), cellVector_.end(), tmp.begin(),
//                     std::bind1st(&Cell::shape, std::mem_fun(&Shape::domainSize));
    for (uint i = 0; i < this->cellCount(); i ++) {
        tmp[i] = cellVector_[i]->shape().domainSize();
    }
    return tmp;
}

void Mesh::sortNodes(const std::vector < int > & perm){

    for (uint i = 0; i < nodeVector_.size(); i ++) nodeVector_[i]->setId(perm[i]);
  //    sort(nodeVector_.begin(), nodeVector_.end(), std::less< int >(mem_fun(&BaseEntity::id)));
    sort(nodeVector_.begin(), nodeVector_.end(), lesserId< Node >);
    recountNodes();
}

void Mesh::recountNodes(){
    for (uint i = 0; i < nodeVector_.size(); i ++) nodeVector_[i]->setId(i);
}

void Mesh::createClosedGeometry(const std::vector < RVector3 > & vPos, int nSegments, double dxInner){
    THROW_TO_IMPL
    //this function should not be part of mesh
    //EAMeshWrapper eamesh(vPos, nSegments, dxInner, *this);
}

void Mesh::createClosedGeometryParaMesh(const std::vector < RVector3 > & vPos, int nSegments, double dxInner){
    createClosedGeometry(vPos, nSegments, dxInner);
    createNeighbourInfos();
    for (uint i = 0; i < cellCount(); i ++) cell(i).setMarker(i);
}

void Mesh::createClosedGeometryParaMesh(const std::vector < RVector3 > & vPos, int nSegments,
                                         double dxInner, const std::vector < RVector3 > & addit){
    THROW_TO_IMPL
    //this function should not be part of meshEntities
//   EAMeshWrapper eamesh;
//   eamesh.createMesh(vPos, nSegments, dxInner, addit);
//   eamesh.mesh(*this);
//   createNeighbourInfos();
//   for (uint i = 0; i < cellCount(); i ++) cell(i).setMarker(i);
}

Mesh Mesh::createH2() const {
    Mesh ret(this->dimension());
    ret.createRefined_(*this, false, true);
    ret.setCellAttributes(ret.cellMarker());
    return ret;
}

Mesh Mesh::createP2() const {
    Mesh ret(this->dimension());
    ret.createRefined_(*this, true, false);
    return ret;
}

int markerT(Node * n0, Node * n1){
    if (n0->marker() == -99 && n1->marker() == -99) return -1;
    if (n0->marker() == -99) return n1->marker();
    if (n1->marker() == -99) return n0->marker();
    if (n0->marker() == n1->marker()) return n1->marker();
    else return 0;
}

Node * Mesh::createRefinementNode_(Node * n0, Node * n1, std::map< std::pair < Index, Index >, Node * > & nodeMatrix){
    Node * n = nodeMatrix[std::make_pair(n0->id(), n1->id())];

    if (!n){
        if (n0 == n1){
            n = this->createNode(n0->pos(), n0->marker()) ;
            nodeMatrix[std::make_pair(n0->id(), n0->id())] = n;
        } else {
            n = this->createNode((n0->pos() + n1->pos()) / 2.0, markerT(n0, n1));
            nodeMatrix[std::make_pair(n0->id(), n1->id())] = n;
            nodeMatrix[std::make_pair(n1->id(), n0->id())] = n;
        }
    }
    return n;
}

void Mesh::createRefined_(const Mesh & mesh, bool p2, bool h2){

    this->clear();

    std::map< std::pair < Index, Index >, Node * > nodeMatrix;

    for (Index i = 0, imax = mesh.nodeCount(); i < imax; i ++) {
        this->createRefinementNode_(&mesh.node(i), &mesh.node(i), nodeMatrix);
    }

    std::vector < Node * > n;
    for (Index i = 0, imax = mesh.cellCount(); i < imax; i ++){
        const Cell & c = mesh.cell(i);

        switch (c.rtti()){
            case MESH_EDGE_CELL_RTTI:
                n.resize(3);
                n[0] = &node(mesh.cell(i).node(0).id());
                n[1] = &node(mesh.cell(i).node(1).id());
                n[2] = createRefinementNode_(n[0], n[1], nodeMatrix);

                if (h2){
                    std::vector < Node * > e1(2);
                    e1[0] = n[0]; e1[1] = n[2];
                    this->createCell(e1, c.marker());
                    e1[0] = n[2]; e1[1] = n[1];
                    this->createCell(e1, c.marker());
                }
                break;
            case MESH_TRIANGLE_RTTI:
                n.resize(6);
                n[0] = &node(mesh.cell(i).node(0).id());
                n[1] = &node(mesh.cell(i).node(1).id());
                n[2] = &node(mesh.cell(i).node(2).id());

                n[3] = createRefinementNode_(n[0], n[1], nodeMatrix);
                n[4] = createRefinementNode_(n[1], n[2], nodeMatrix);
                n[5] = createRefinementNode_(n[2], n[0], nodeMatrix);

                if (h2){
                    this->createTriangle(*n[0], *n[3], *n[5], c.marker());
                    this->createTriangle(*n[1], *n[4], *n[3], c.marker());
                    this->createTriangle(*n[2], *n[5], *n[4], c.marker());
                    this->createTriangle(*n[3], *n[4], *n[5], c.marker());
                }

                break;
            case MESH_QUADRANGLE_RTTI:
                n.resize(8);
                n[0] = &node(mesh.cell(i).node(0).id());
                n[1] = &node(mesh.cell(i).node(1).id());
                n[2] = &node(mesh.cell(i).node(2).id());
                n[3] = &node(mesh.cell(i).node(3).id());

                n[4] = createRefinementNode_(n[0], n[1], nodeMatrix);
                n[5] = createRefinementNode_(n[1], n[2], nodeMatrix);
                n[6] = createRefinementNode_(n[2], n[3], nodeMatrix);
                n[7] = createRefinementNode_(n[3], n[0], nodeMatrix);

                if (h2){
                    Node *n8 = this->createNode(c.shape().xyz(RVector3(0.5, 0.5)));
                    this->createQuadrangle(*n[0], *n[4], *n8, *n[7], c.marker());
                    this->createQuadrangle(*n[1], *n[5], *n8, *n[4], c.marker());
                    this->createQuadrangle(*n[2], *n[6], *n8, *n[5], c.marker());
                    this->createQuadrangle(*n[3], *n[7], *n8, *n[6], c.marker());
                }

                break;
            case MESH_TETRAHEDRON_RTTI:
                n.resize(10);
                if (oldTet10NumberingStyle_){

                    for (Index j = 0; j < n.size(); j ++) {

                        n[j] = createRefinementNode_(& this->node(c.node(Tet10NodeSplitZienk[j][0]).id()),
                                                        & this->node(c.node(Tet10NodeSplitZienk[j][1]).id()),
                                                        nodeMatrix);
                    }

                    if (h2){
                        this->createTetrahedron(*n[4], *n[6], *n[5], *n[0], c.marker());
                        this->createTetrahedron(*n[4], *n[5], *n[6], *n[9], c.marker());
                        this->createTetrahedron(*n[7], *n[9], *n[4], *n[1], c.marker());
                        this->createTetrahedron(*n[7], *n[4], *n[9], *n[5], c.marker());
                        this->createTetrahedron(*n[8], *n[7], *n[5], *n[2], c.marker());
                        this->createTetrahedron(*n[8], *n[5], *n[7], *n[9], c.marker());
                        this->createTetrahedron(*n[6], *n[9], *n[8], *n[3], c.marker());
                        this->createTetrahedron(*n[6], *n[8], *n[9], *n[5], c.marker());
                    }

                } else {
                    for (Index j = 0; j < n.size(); j ++) {
                        n[j] = createRefinementNode_(& this->node(c.node(Tet10NodeSplit[j][0]).id()),
                                                        & this->node(c.node(Tet10NodeSplit[j][1]).id()),
                                                        nodeMatrix);
                    }

                    if (h2){
                        THROW_TO_IMPL
                    }
                }

                break;
            case MESH_HEXAHEDRON_RTTI:
                n.resize(20);
                for (Index j = 0; j < n.size(); j ++) {
                    n[j] = createRefinementNode_(& this->node(c.node(Hex20NodeSplit[j][0]).id()),
                                                 & this->node(c.node(Hex20NodeSplit[j][1]).id()),
                                                 nodeMatrix);
                }
                if (h2){
/* 27 new nodes 3 x 9 = 8 nodes + 12 edges + 6 facets + 1 center                     
          7-----14------6  \n
         /|            /|  \n
        / |           / |  \n
      15 19  -21-    13 18 \n
      /   |     24  /   |  \n
     /    |        /    |  \n
    4-----12------5  23 |  \n
    | 25  3-----10|-----2  \n
    |    /        |    /   \n
   16   / -22-    17  /    \n
    | 11    -20-  |  9     \n
    | /           | /      \n
    |/            |/       \n
    0------8------1        \n

*/
                    Node *n20 = createRefinementNode_(n[8], n[10], nodeMatrix);
                    Node *n21 = createRefinementNode_(n[12], n[14], nodeMatrix);
                    Node *n22 = createRefinementNode_(n[8], n[12], nodeMatrix);
                    Node *n23 = createRefinementNode_(n[9], n[13], nodeMatrix);
                    Node *n24 = createRefinementNode_(n[10], n[14], nodeMatrix);
                    Node *n25 = createRefinementNode_(n[11], n[15], nodeMatrix);

                    Node *n26 = createRefinementNode_(n20, n21, nodeMatrix);

                    std::vector < Node* > ns(8);
                    Node *n1_[]={ n[0], n[8], n20, n[11], n[16], n22, n26, n25 }; std::copy(&n1_[0], &n1_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n2_[]= { n[8], n[1], n[9], n20, n22, n[17], n23, n26 };  std::copy(&n2_[0], &n2_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n3_[]= { n[11], n20, n[10], n[3], n25, n26, n24, n[19] };  std::copy(&n3_[0], &n3_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n4_[]= { n20, n[9], n[2], n[10], n26, n23, n[18], n24 };  std::copy(&n4_[0], &n4_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n5_[]= { n[16], n22, n26, n25, n[4], n[12], n21, n[15] };  std::copy(&n5_[0], &n5_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n6_[]= { n22, n[17], n23, n26, n[12], n[5], n[13], n21 };  std::copy(&n6_[0], &n6_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n7_[]= { n25, n26, n24, n[19], n[15], n21, n[14], n[7] };  std::copy(&n7_[0], &n7_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n8_[]= { n26, n23, n[18], n24, n21, n[13], n[6], n[14] };  std::copy(&n8_[0], &n8_[8], &ns[0]);
                    this->createCell(ns, c.marker());

                }

                break;
            case MESH_TRIPRISM_RTTI:
                n.resize(15);
                for (Index j = 0; j < n.size(); j ++) {
                    n[j] = createRefinementNode_(& this->node(c.node(Prism15NodeSplit[j][0]).id()),
                                                 & this->node(c.node(Prism15NodeSplit[j][1]).id()),
                                                 nodeMatrix);
                }
                if (h2){

                    Node *nf1 = createRefinementNode_(n[6], n[9], nodeMatrix);
                    Node *nf2 = createRefinementNode_(n[7], n[10], nodeMatrix);
                    Node *nf3 = createRefinementNode_(n[8], n[11], nodeMatrix);

                    std::vector < Node* > ns(6);
                    Node *n1_[]={ n[0], n[6], n[8], n[12], nf1, nf3 }; std::copy(&n1_[0], &n1_[6], &ns[0]);
                    this->createCell(ns, c.marker());
                    Node *n2_[]={ n[1], n[7], n[6], n[13], nf2, nf1 }; std::copy(&n2_[0], &n2_[6], &ns[0]);
                    this->createCell(ns, c.marker());
                    Node *n3_[]={ n[2], n[8], n[7], n[14], nf3, nf2 }; std::copy(&n3_[0], &n3_[6], &ns[0]);
                    this->createCell(ns, c.marker());
                    Node *n4_[]={ n[6], n[7], n[8], nf1, nf2, nf3 }; std::copy(&n4_[0], &n4_[6], &ns[0]);
                    this->createCell(ns, c.marker());

                    Node *n5_[]={ n[12], nf1, nf3, n[3], n[9], n[11] }; std::copy(&n5_[0], &n5_[6], &ns[0]);
                    this->createCell(ns, c.marker());
                    Node *n6_[]={ n[13], nf2, nf1, n[4], n[10], n[9] }; std::copy(&n6_[0], &n6_[6], &ns[0]);
                    this->createCell(ns, c.marker());
                    Node *n7_[]={ n[14], nf3, nf2, n[5], n[11], n[10] }; std::copy(&n7_[0], &n7_[6], &ns[0]);
                    this->createCell(ns, c.marker());
                    Node *n8_[]={ nf1, nf2, nf3, n[9], n[10], n[11] }; std::copy(&n8_[0], &n8_[6], &ns[0]);
                    this->createCell(ns, c.marker());

                }
                break;
            case MESH_PYRAMID_RTTI:
                n.resize(13);
                for (Index j = 0; j < n.size(); j ++) {
                    n[j] = createRefinementNode_(& this->node(c.node(Pyramid13NodeSplit[j][0]).id()),
                                                    & this->node(c.node(Pyramid13NodeSplit[j][1]).id()),
                                                    nodeMatrix);
                }
                if (h2){
                    THROW_TO_IMPL
                }
                break;
            default: std::cerr << c.rtti() <<" " << std::endl; THROW_TO_IMPL  break;
        }

        if (p2 && !h2){
            createCell(n, c.marker());
        }

    } // for_each cell

    for (Index i = 0; i < mesh.boundaryCount(); i++){

        const Boundary & b = mesh.boundary(i);

        switch (b.rtti()){
            case MESH_BOUNDARY_NODE_RTTI:
                n.resize(1);
                n[0] = &node(b.node(0).id());
                break;
            case MESH_EDGE_RTTI:
                n.resize(3);
                n[0] = &node(b.node(0).id());
                n[1] = &node(b.node(1).id());
                n[2] = createRefinementNode_(n[0], n[1], nodeMatrix);
                break;
            case MESH_TRIANGLEFACE_RTTI:
                n.resize(6);
                n[0] = &node(b.node(0).id());
                n[1] = &node(b.node(1).id());
                n[2] = &node(b.node(2).id());

                n[3] = createRefinementNode_(n[0], n[1], nodeMatrix);
                n[4] = createRefinementNode_(n[1], n[2], nodeMatrix);
                n[5] = createRefinementNode_(n[2], n[0], nodeMatrix);
                break;
            case MESH_QUADRANGLEFACE_RTTI:
                n.resize(8);
                n[0] = &node(b.node(0).id());
                n[1] = &node(b.node(1).id());
                n[2] = &node(b.node(2).id());
                n[3] = &node(b.node(3).id());

                n[4] = createRefinementNode_(n[0], n[1], nodeMatrix);
                n[5] = createRefinementNode_(n[1], n[2], nodeMatrix);
                n[6] = createRefinementNode_(n[2], n[3], nodeMatrix);
                n[7] = createRefinementNode_(n[3], n[0], nodeMatrix);
                break;
            default: std::cerr << b.rtti() <<" " << std::endl; THROW_TO_IMPL  break;
        }

        if (p2 && !h2){
            createBoundary(n, b.marker());
        } else {
            switch (b.rtti()){
                case MESH_BOUNDARY_NODE_RTTI:
                    this->createBoundary(n, b.marker());
                    break;
                case MESH_EDGE_RTTI:
                    this->createEdge(*n[0], *n[2], b.marker());
                    this->createEdge(*n[2], *n[1], b.marker());
                    break;
                case MESH_TRIANGLEFACE_RTTI:
                    this->createTriangleFace(*n[0], *n[3], *n[5], b.marker());
                    this->createTriangleFace(*n[1], *n[4], *n[3], b.marker());
                    this->createTriangleFace(*n[2], *n[5], *n[4], b.marker());
                    this->createTriangleFace(*n[3], *n[4], *n[5], b.marker());
                    break;
                case MESH_QUADRANGLEFACE_RTTI:
                    /* 
                     * 3---6---2
                     * |   |   |            
                     * 7---8---5
                     * |   |   |
                     * 0---4---1
                    */
                    Node *n8 = nodeMatrix[std::make_pair(n[4]->id(), n[6]->id())];
                    if (!n8) n8 = createRefinementNode_(n[5], n[7], nodeMatrix);

                    this->createQuadrangleFace(*n[0], *n[4], *n8, *n[7], b.marker());
                    this->createQuadrangleFace(*n[1], *n[5], *n8, *n[4], b.marker());
                    this->createQuadrangleFace(*n[2], *n[6], *n8, *n[5], b.marker());
                    this->createQuadrangleFace(*n[3], *n[7], *n8, *n[6], b.marker());

                    break;

            }
        } // if not p2
    } // for_each boundary
}

// void Mesh::createH2(const Mesh & mesh){
//     std::vector < int > cellsToRefine(mesh.cellCount());
//
//     for (uint i = 0, imax = mesh.cellCount(); i < imax; i ++) cellsToRefine[i] = i;
//     createRefined(mesh, cellsToRefine);
// }

// int Mesh::createRefined(const Mesh & mesh, const std::vector < int > & cellIdx){
//
//     clear();
//     this->setDimension(mesh.dim());
//     bool needCellSplit = false;
//     if (dimension_ == 1) {
//         CERR_TO_IMPL
//     } else { // dimension != 1
//         for (uint i = 0; i < cellIdx.size(); i ++){
//             switch(mesh.cell(i).rtti()){
//                 case MESH_TRIANGLE_RTTI:
//                 case MESH_TETRAHEDRON_RTTI: break;
//                 case MESH_QUADRANGLE_RTTI:
//                 case MESH_HEXAHEDRON_RTTI: needCellSplit = true; break;
//                 default:
//                 std::cerr << WHERE_AM_I << " cell type for refinement not yet implemented " << mesh.cell(i).rtti() << std::endl;
//             }
//         }
//
//         if (needCellSplit){
//             Mesh tmpMesh(dimension_);
//             for (uint i = 0; i < mesh.nodeCount(); i ++) tmpMesh.createNode(mesh.node(i));
//
//             Cell * cell;
//             for (uint i = 0; i < mesh.cellCount(); i ++) {
//                 cell = &mesh.cell(i);
//                 switch(mesh.cell(i).rtti()){
//                 case MESH_TRIANGLE_RTTI:
//                 case MESH_TETRAHEDRON_RTTI: tmpMesh.createCell(*cell); break;
//                 case MESH_QUADRANGLE_RTTI:
//                     tmpMesh.createTriangle(tmpMesh.node(cell->node(0).id()),
//                                     tmpMesh.node(cell->node(1).id()),
//                                     tmpMesh.node(cell->node(2).id()),
//                                     cell->marker());
//                     tmpMesh.cell(tmpMesh.cellCount() - 1).setAttribute(cell->attribute());
//                     tmpMesh.createTriangle(tmpMesh.node(cell->node(0).id()),
//                                     tmpMesh.node(cell->node(2).id()),
//                                     tmpMesh.node(cell->node(3).id()),
//                                     mesh.cell(i).marker());
//                     tmpMesh.cell(tmpMesh.cellCount() - 1).setAttribute(cell->attribute());
//                     break;
//                 case MESH_HEXAHEDRON_RTTI:
//                    for (uint j = 0; j < 6; j ++){
//                         tmpMesh.createTetrahedron(
//                             tmpMesh.node(cell->node(HexahedronSplit6TetID[j][0]).id()),
//                             tmpMesh.node(cell->node(HexahedronSplit6TetID[j][1]).id()),
//                             tmpMesh.node(cell->node(HexahedronSplit6TetID[j][2]).id()),
//                             tmpMesh.node(cell->node(HexahedronSplit6TetID[j][3]).id()),
//                             cell->marker());
//                         tmpMesh.cell(tmpMesh.cellCount() - 1).setAttribute(cell->attribute());
//                     }
//                     break;
//                 }
//             }
//
//             tmpMesh.createNeighbourInfos();
//
//             for (uint i = 0; i < mesh.boundaryCount(); i ++){
//                 Boundary * bound = &mesh.boundary(i);
//                 if (bound->marker() != 0){
//                     switch (bound->rtti()){
//                         case MESH_EDGE_RTTI:{
//                             Boundary *b = NULL;
//                             b = findBoundary(tmpMesh.node(bound->node(0).id()),
//                                               tmpMesh.node(bound->node(1).id()));
//                             if (b) b->setMarker(bound->marker());
//                         } break;
//                         case MESH_TRIANGLEFACE_RTTI:
//                             THROW_TO_IMPL
//                             break;
//                         case MESH_QUADRANGLEFACE_RTTI: {
//                             Boundary *b = NULL;
//                             b = findBoundary(tmpMesh.node(bound->node(0).id()),
//                                               tmpMesh.node(bound->node(1).id()),
//                                               tmpMesh.node(bound->node(2).id()));
//                             if (b) b->setMarker(bound->marker());
//                             b = findBoundary(tmpMesh.node(bound->node(1).id()),
//                                               tmpMesh.node(bound->node(2).id()),
//                                               tmpMesh.node(bound->node(3).id()));
//                             if (b) b->setMarker(bound->marker());
//                             b = findBoundary(tmpMesh.node(bound->node(0).id()),
//                                               tmpMesh.node(bound->node(1).id()),
//                                               tmpMesh.node(bound->node(3).id()));
//                             if (b) b->setMarker(bound->marker());
//                             b = findBoundary(tmpMesh.node(bound->node(0).id()),
//                                               tmpMesh.node(bound->node(2).id()),
//                                               tmpMesh.node(bound->node(3).id()));
//                             if (b) b->setMarker(bound->marker());
//                         } break;
//                     }
//                 }
//             }
//
//             if (dimension_ == 3){
//                 this->copy_(tmpMesh);
//                 return 0;
//             } else {
//                 this->copy_(tmpMesh);
//                // return 0;
//                 std::vector < int > c(tmpMesh.cellCount());
//                 for (uint i = 0, imax = c.size(); i < imax; i ++) c[i] = i;
//                 return createRefined(tmpMesh, c);
//             }
//
//         } // else need split
//     } // else dimension_ != 1
//
//     if (dimension_ == 2) return createRefined2D_(mesh, cellIdx);
//     else return createRefined3D_(mesh, cellIdx);
//
//     return 0;
// }


// int Mesh::createRefined2D_(const Mesh & mesh, const std::vector < int > & cellIdx){
//     // alle OrginalKnoten werden kopiert
//     nodeVector_.reserve(mesh.nodeCount());
//     for (uint i = 0, imax = mesh.nodeCount(); i < imax; i ++) createNode(mesh.node(i));
//
//     Node *n0 = NULL, *n1 = NULL, *n2 = NULL, *n3 = NULL, *n4 = NULL; Node *n5 = NULL;
//
//     //SparseMapMatrix < Node *, Index > nodeMatrix(nodeCount(), nodeCount());
//     std::map< std::pair < Index, Index >, Node * > nodeMatrix;
//
//     //** create nodes for all for refinement selected cell;
//     for (int i = 0, imax = cellIdx.size(); i < imax; i ++){
//         n0 = &node(mesh.cell(cellIdx[i]).node(0).id());
//         n1 = &node(mesh.cell(cellIdx[i]).node(1).id());
//         n2 = &node(mesh.cell(cellIdx[i]).node(2).id());
//
//         n3 = createRefinementNode_(n0, n1, nodeMatrix);
//         n4 = createRefinementNode_(n1, n2, nodeMatrix);
//         n5 = createRefinementNode_(n2, n0, nodeMatrix);
//     }
//
//     Boundary * edge = NULL;
//     int marker = 0;
//     for (uint i = 0; i < mesh.boundaryCount(); i++){
//         edge = findBoundary(mesh.boundary(i).node(0), mesh.boundary(i).node(1));
//
//         if (edge != NULL){
//             n0 = &node(mesh.boundary(i).node(0).id());
//             n1 = &node(mesh.boundary(i).node(1).id());
//             n3 = createRefinementNode_(n0, n1, nodeMatrix);
//
//             marker = edge->marker();
//
//             if (n3 != NULL){
//                 createEdge(*n0, *n3, marker);
//                 createEdge(*n3, *n1, marker);
//             } else {
//                 createEdge(*n0, *n1, marker);
//             }
//
//             // the marker of new created nodes, will be derived from its neighbors if there markers are greater 0
//             if (marker != 0){
//                 if (n0->marker() > 0) n0->setMarker(marker);
//                 if (n1->marker() > 0) n1->setMarker(marker);
//                 if (n3 != NULL) n3->setMarker(marker);
//             }
//         } // edge != NULL
//     } // for each boundary
//
//     //** create all new cells
//     for (int i = 0, imax = mesh.cellCount(); i < imax; i ++){
//         n0 = &node(mesh.cell(i).node(0).id());
//         n1 = &node(mesh.cell(i).node(1).id());
//         n2 = &node(mesh.cell(i).node(2).id());
//
//         n3 = createRefinementNode_(n0, n1, nodeMatrix);
//         n4 = createRefinementNode_(n1, n2, nodeMatrix);
//         n5 = createRefinementNode_(n2, n0, nodeMatrix);
//
//         if (n3 == NULL && n4 == NULL && n5 == NULL){
//             createTriangle(*n0, *n1, *n2, mesh.cell(i).marker());
//         } else if (n3 != NULL && n4 == NULL && n5 == NULL) {
//             createTriangle(*n3, *n2, *n0, mesh.cell(i).marker());
//             createTriangle(*n3, *n1, *n2, mesh.cell(i).marker());
//             createEdge(*n3, *n2);
//         } else if (n3 == NULL && n4 != NULL && n5 == NULL) {
//             createTriangle(*n4, *n2, *n0, mesh.cell(i).marker());
//             createTriangle(*n4, *n0, *n1, mesh.cell(i).marker());
//             createEdge(*n4, *n0);
//         } else if (n3 == NULL && n4 == NULL && n5 != NULL) {
//             createTriangle(*n5, *n1, *n2, mesh.cell(i).marker());
//             createTriangle(*n5, *n0, *n1, mesh.cell(i).marker());
//             createEdge(*n5, *n1);
//         } else if (n3 != NULL && n4 != NULL && n5 == NULL) {
//             createTriangle(*n3, *n1, *n4, mesh.cell(i).marker());
//             createEdge(*n3, *n4);
//             if (n3->dist(*n2) < n4->dist(*n0)){
//                 createTriangle(*n3, *n4, *n2, mesh.cell(i).marker());
//                 createTriangle(*n3, *n2, *n0, mesh.cell(i).marker());
//                 createEdge(*n2, *n3);
//             } else {
//                 createTriangle(*n4, *n0, *n3, mesh.cell(i).marker());
//                 createTriangle(*n4, *n2, *n0, mesh.cell(i).marker());
//                 createEdge(*n0, *n4);
//             }
//         } else if (n3 == NULL && n4 != NULL && n5 != NULL) {
//             createTriangle(*n4, *n2, *n5, mesh.cell(i).marker());
//             createEdge(*n4, *n5);
//
//             if (n4->dist(*n0) < n5->dist(*n1)){
//                 createTriangle(*n4, *n5, *n0, mesh.cell(i).marker());
//                 createTriangle(*n4, *n0, *n1, mesh.cell(i).marker());
//                 createEdge(*n0, *n4);
//             } else {
//                 createTriangle(*n4, *n5, *n1, mesh.cell(i).marker());
//                 createTriangle(*n5, *n0, *n1, mesh.cell(i).marker());
//                 createEdge(*n1, *n5);
//             }
//         } else if (n3 != NULL && n4 == NULL && n5 != NULL) {
//             createTriangle(*n5, *n0, *n3, mesh.cell(i).marker());
//             createEdge(*n3, *n5);
//             if (n3->dist(*n2) < n5->dist(*n1)){
//                 createTriangle(*n5, *n3, *n2, mesh.cell(i).marker());
//                 createTriangle(*n3, *n1, *n2, mesh.cell(i).marker());
//                 createEdge(*n2, *n3);
//             } else {
//                 createTriangle(*n5, *n3, *n1, mesh.cell(i).marker());
//                 createTriangle(*n5, *n1, *n2, mesh.cell(i).marker());
//                 createEdge(*n1, *n5);
//             }
//         } else if (n3 != NULL && n4 != NULL && n5 != NULL) {
//             createTriangle(*n0, *n3, *n5, mesh.cell(i).marker());
//             createTriangle(*n1, *n4, *n3, mesh.cell(i).marker());
//             createTriangle(*n2, *n5, *n4, mesh.cell(i).marker());
//             createTriangle(*n3, *n4, *n5, mesh.cell(i).marker());
//             createEdge(*n3, *n4);
//             createEdge(*n4, *n5);
//             createEdge(*n5, *n3);
//         }
//     }
//     return 1;
// }

// int Mesh::createRefined3D_(const Mesh & mesh, const std::vector < int > & cellIdx){
//     nodeVector_.reserve(mesh.nodeCount());
//
//     for (int i = 0, imax = mesh.nodeCount(); i < imax; i ++) createNode(mesh.node(i));
//
//     Node *n0 = NULL, *n1 = NULL, *n2 = NULL, *n3 = NULL, *n4 = NULL;
//     Node *n5 = NULL, *n6 = NULL, *n7 = NULL, *n8 = NULL, *n9 = NULL;
//     Boundary * face = NULL;
//
//     //SparseMapMatrix < Node *, Index > nodeMatrix(nodeCount(), nodeCount());
//     std::map< std::pair < Index, Index >, Node * > nodeMatrix;
//
//     for (int i = 0, imax = cellIdx.size(); i < imax; i ++){
//     //        std::cout << "boundcount: " << cellIdx.size() << " " << this->boundaryCount() << " " << mesh.boundaryCount()<< std::endl;
//         n0 = &node(mesh.cell(cellIdx[i]).node(0).id());
//         n1 = &node(mesh.cell(cellIdx[i]).node(1).id());
//         n2 = &node(mesh.cell(cellIdx[i]).node(2).id());
//         n3 = &node(mesh.cell(cellIdx[i]).node(3).id());
//
//         n4 = createRefinementNode_(n0, n1, nodeMatrix);
//         n5 = createRefinementNode_(n0, n2, nodeMatrix);
//         n6 = createRefinementNode_(n0, n3, nodeMatrix);
//         n7 = createRefinementNode_(n1, n2, nodeMatrix);
//         n8 = createRefinementNode_(n2, n3, nodeMatrix);
//         n9 = createRefinementNode_(n1, n3, nodeMatrix);
//
//
// //     if ((n4 = nodeMatrix[n0->id()][n1->id()]) == NULL){
// //       n4 = createNode((n0->pos() + n1->pos()) / 2.0, markerT(n0, n1));
// //       nodeMatrix[n0->id()][n1->id()] = n4 ;
// //       nodeMatrix[n1->id()][n0->id()] = n4 ;
// //     }
// //     if ((n5 = nodeMatrix[n0->id()][n2->id()]) == NULL){
// //       n5 = createNode((n0->pos() + n2->pos()) / 2.0, markerT(n0, n2));
// //       nodeMatrix[n0->id()][n2->id()] = n5 ;
// //       nodeMatrix[n2->id()][n0->id()] = n5 ;
// //     }
// //     if ((n6 = nodeMatrix[n0->id()][n3->id()]) == NULL){
// //       n6 = createNode((n0->pos() + n3->pos()) / 2.0, markerT(n0, n3));
// //       nodeMatrix[n0->id()][n3->id()] = n6;
// //       nodeMatrix[n3->id()][n0->id()] = n6;
// //     }
// //     if ((n7 = nodeMatrix[n1->id()][n2->id()]) == NULL){
// //       n7 = createNode((n1->pos() + n2->pos()) / 2.0, markerT(n1, n2));
// //       nodeMatrix[n1->id()][n2->id()] = n7;
// //       nodeMatrix[n2->id()][n1->id()] = n7;
// //     }
// //     if ((n8 = nodeMatrix[n2->id()][n3->id()]) == NULL){
// //       n8 = createNode((n2->pos() + n3->pos()) / 2.0, markerT(n2, n3));
// //       nodeMatrix[n2->id()][n3->id()] = n8;
// //       nodeMatrix[n3->id()][n2->id()] = n8;
// //     }
// //     if ((n9 = nodeMatrix[n1->id()][n3->id()]) == NULL){
// //       n9 = createNode((n3->pos() + n1->pos()) / 2.0, markerT(n3, n1));
// //       nodeMatrix[n1->id()][n3->id()] = n9;
// //       nodeMatrix[n3->id()][n1->id()] = n9;
// //     }
//
//     createTetrahedron(*n4, *n6, *n5, *n0, mesh.cell(i).marker());
//     createTetrahedron(*n4, *n5, *n6, *n9, mesh.cell(i).marker());
//
//     createTetrahedron(*n7, *n9, *n4, *n1, mesh.cell(i).marker());
//     createTetrahedron(*n7, *n4, *n9, *n5, mesh.cell(i).marker());
//
//     createTetrahedron(*n8, *n7, *n5, *n2, mesh.cell(i).marker());
//     createTetrahedron(*n8, *n5, *n7, *n9, mesh.cell(i).marker());
//
//     createTetrahedron(*n6, *n9, *n8, *n3, mesh.cell(i).marker());
//     createTetrahedron(*n6, *n8, *n9, *n5, mesh.cell(i).marker());
//
//     face = findBoundary(mesh.cell(i).node(0), mesh.cell(i).node(1), mesh.cell(i).node(2));
//     if (face != NULL){
//       if (face->marker() != 0) {
//         createTriangleFace(*n0, *n5, *n4, face->marker());
//         createTriangleFace(*n1, *n4, *n7, face->marker());
//         createTriangleFace(*n2, *n7, *n5, face->marker());
//         createTriangleFace(*n4, *n5, *n7, face->marker());
//       }
//     }
//     face = findBoundary(mesh.cell(i).node(0), mesh.cell(i).node(1), mesh.cell(i).node(3));
//     if (face != NULL){
//       if (face->marker() != 0) {
//         createTriangleFace(*n0, *n4, *n6, face->marker());
//         createTriangleFace(*n1, *n9, *n4, face->marker());
//         createTriangleFace(*n3, *n6, *n9, face->marker());
//         createTriangleFace(*n4, *n9, *n6, face->marker());
//       }
//     }
//     face = findBoundary(mesh.cell(i).node(1), mesh.cell(i).node(2), mesh.cell(i).node(3));
//     if (face != NULL){
//       if (face->marker() != 0) {
//         createTriangleFace(*n1, *n7, *n9, face->marker());
//         createTriangleFace(*n2, *n8, *n7, face->marker());
//         createTriangleFace(*n3, *n9, *n8, face->marker());
//         createTriangleFace(*n7, *n8, *n9, face->marker());
//       }
//     }
//     face = findBoundary(mesh.cell(i).node(2), mesh.cell(i).node(0), mesh.cell(i).node(3));
//     if (face != NULL){
//       if (face->marker() != 0) {
//         createTriangleFace(*n2, *n5, *n8, face->marker());
//         createTriangleFace(*n3, *n8, *n6, face->marker());
//         createTriangleFace(*n0, *n6, *n5, face->marker());
//         createTriangleFace(*n8, *n5, *n6, face->marker());
//       }
//     }
//   }
//
// return 1;
// }
    
void Mesh::cleanNeighbourInfos(){
    //std::cout << "Mesh::cleanNeighbourInfos()"<< std::endl;
    for (uint i = 0; i < cellCount(); i ++){
        cell(i).cleanNeighbourInfos();
    }
    for (uint i = 0; i < boundaryCount(); i ++){
        boundary(i).setLeftCell(NULL);
        boundary(i).setRightCell(NULL);
    }
}

void Mesh::createNeighbourInfos(bool force){
//     double med = 0.;
    if (!neighboursKnown_ || force){
        this->cleanNeighbourInfos();

//         Stopwatch sw(true);
        
        for (uint i = 0; i < cellCount(); i ++){
            
            createNeighbourInfosCell_(&cell(i));
//             med+=sw.duration(true);
        }
        neighboursKnown_ = true;
    }
    
//     std::cout << med << " " << med/cellCount() << std::endl;
}

void Mesh::createNeighbourInfosCell_(Cell *c){
    
    for (uint j = 0; j < c->boundaryCount(); j++){
        if (c->neighbourCell(j)) continue;
        
        c->findNeighbourCell(j);
        std::vector < Node * > nodes(c->boundaryNodes(j));
//                 std::cout << findBoundary(nodes) << std::endl;

 
        Boundary * bound = createBoundary(nodes, 0);

//         Boundary * bound = findBoundary(*nodes[0], *nodes[1], *nodes[2]);
//         Boundary * bound = findBoundary(nodes);
//         if (!bound) {
//             bound = createBoundary_< TriangleFace >(nodes, 0, boundaryCount());
//         }
            
        bool cellIsLeft = true;
        if (bound->shape().nodeCount() == 2) {
            cellIsLeft = (c->boundaryNodes(j)[0]->id() == bound->node(0).id());
        } else if (bound->shape().nodeCount() > 2) {
            // normvector of boundary shows outside for left cell ... every bound need leftcell
            if (bound->normShowsOutside(*c)){
                cellIsLeft = true;
            } else {
                cellIsLeft = false;
            }
        }

        if (bound->leftCell() == NULL && cellIsLeft) {
            if (bound->rightCell() == c){
                //* we were already here .. no need to do it again
                continue;
            }
            bound->setLeftCell(c);
            if (c->neighbourCell(j) && bound->rightCell() == NULL) bound->setRightCell(c->neighbourCell(j));
            
        } else if (bound->rightCell() == NULL){
            if (bound->leftCell() == c){
                //* we were already here .. no need to do it again
                continue;
            } else { 
                bound->setRightCell(c);
                if (c->neighbourCell(j) && bound->leftCell() == NULL ) bound->setLeftCell(c->neighbourCell(j));
            }
        }
                   
//         if (!bound->leftCell()){
//             std::cout << *bound << " " << bound->leftCell() << " " << *bound->rightCell() << std::endl;
//             throwError(1, WHERE + " Ooops, crosscheck -- every boundary need left cell.");
//         }
                   
//         std::cout << bound->id() << " " << bound->leftCell() << " " << bound->rightCell() << std::endl;
            
        //** cross check;
        if (((bound->leftCell() != c) && (bound->rightCell() != c)) || 
            (bound->leftCell() == bound->rightCell())){
            std::cerr << *c << std::endl;
            std::cerr << *bound << std::endl;
            std::cerr << bound->leftCell() << " " << bound->rightCell() << std::endl;
            throwError(1, WHERE + " Ooops, crosscheck --this should not happen.");
        } else {
//                     std::cout << nBounds << std::endl;
//                     std::cerr << bound->leftCell() << " " << bound->rightCell() << std::endl;
        }
    } // for_each boundary in cell
}

void Mesh::create1DGrid(const RVector & x){
    this->clear();
    this->setDimension(1);
    if (unique(sort(x)).size() != x.size()) {
        std::cerr << WHERE_AM_I << "Warning! there are non-unique values in pos" << std::endl;
    }

    if (x.size() > 1){
        this->createNode(x[0], 0.0, 0.0);
        for (uint i = 1; i < x.size(); i ++){
            this->createNode(x[i], 0.0, 0.0);
            std::vector < Node * > nodes(2);
            nodes[0] = & node(nodeCount() - 2);
            nodes[1] = & node(nodeCount() - 1);
            this->createCell(nodes);
        }
        this->createNeighbourInfos();

        for (Index i = 0; i < boundaryCount(); i ++){
            if (boundary(i).leftCell() == NULL || boundary(i).rightCell() == NULL){
                if (boundary(i).node(0).pos()[0] == x[0]) boundary(i).setMarker(1);
                else if (boundary(i).node(0).pos()[0] == x[x.size()-1]) boundary(i).setMarker(2);
            }
        }

    } else {
        std::cerr << WHERE_AM_I << "Warning! there are too few positions given: "
                << x.size() << std::endl;
    }
}

void Mesh::create2DGrid(const RVector & x, const RVector & y, int markerType){

    this->clear();
    this->setDimension(2);
    if (unique(sort(x)).size() != x.size()) {
        std::cerr << WHERE_AM_I << "Warning! there are non-unique values in pos" << std::endl;
    }

    if (unique(sort(y)).size() != y.size()) {
        std::cerr << WHERE_AM_I << "Warning! there are non-unique values in pos" << std::endl;
    }

    int marker = 0;
    if (x.size() > 1 && y.size() > 1){
        for (uint i = 0; i < y.size(); i ++){
            if (i > 0 && markerType == 2) marker++;

            for (uint j = 0; j < x.size(); j ++){
                this->createNode(x[j], y[i], 0.0);

                if (i > 0 && j > 0){
                    if (markerType == 1 || markerType == 12) marker++;
                    std::vector < Node * > nodes(4);
                    nodes[3] = & node(this->nodeCount() - 2);
                    nodes[2] = & node(this->nodeCount() - 1);
                    nodes[1] = & node(this->nodeCount() - 1 - x.size());
                    nodes[0] = & node(this->nodeCount() - 2 - x.size());

                    this->createCell(nodes, marker);
                   
//                     this->createTriangle(*nodes[1], *nodes[2], *nodes[3], marker);
//                     this->createTriangle(*nodes[0], *nodes[1], *nodes[3], marker);
                    
                }
            }
            if (markerType == 1) marker = 0;
        }
        this->createNeighbourInfos();

        for (Index i = 0; i < boundaryCount(); i ++){
            if (boundary(i).leftCell() == NULL || boundary(i).rightCell() == NULL){
                //Top
                if (std::abs(boundary(i).norm()[1] - 1.0) < TOLERANCE) boundary(i).setMarker(1);
                // Bottom
                else if (std::abs(boundary(i).norm()[1] + 1.0) < TOLERANCE) boundary(i).setMarker(3);
                // Left
                else if (std::abs(boundary(i).norm()[0] + 1.0) < TOLERANCE) boundary(i).setMarker(2);
                // Right
                else if (std::abs(boundary(i).norm()[0] - 1.0) < TOLERANCE) boundary(i).setMarker(4);
//                 boundary(i).setMarker(1);
            }
        }

    } else {
        std::cerr << WHERE_AM_I << "Warning! there are too few positions given: "
            << x.size() << " " << y.size() << std::endl;
    }
}

void Mesh::create3DGrid(const RVector & x, const RVector & y, const RVector & z, int markerType){

    this->clear();
    this->setDimension(3);
    if (unique(sort(x)).size() != x.size()) {
        std::cerr << WHERE_AM_I << "Warning! there are non-unique values in pos" << std::endl;
    }

    if (unique(sort(y)).size() != y.size()) {
        std::cerr << WHERE_AM_I << "Warning! there are non-unique values in pos" << std::endl;
    }

    if (unique(sort(z)).size() != z.size()) {
        std::cerr << WHERE_AM_I << "Warning! there are non-unique values in pos" << std::endl;
    }

    int marker = 0;
    if (x.size() > 1 && y.size() > 1 && z.size() > 1){
        for (uint k = 0; k < z.size(); k ++){
            
            if (k > 0 && markerType == 3) marker++; //** count only increasing z
            
            for (uint j = 0; j < y.size(); j ++){
                
                if (j > 0 && markerType == 2) marker++;  //** count increasing y or yz
                if (j > 0 && k > 0 && markerType == 23) marker++;  //** count increasing y or yz
                
                for (uint i = 0; i < x.size(); i ++){ //**  count increasing x, yz, xz or xyz
                    
                    this->createNode(x[i], y[j], z[k]);
                    
                    if (i > 0 && j > 0 && k > 0){
                        if (markerType == 1 || markerType == 12 || markerType == 13 || markerType == 123) marker++; //** increasing y

                        std::vector < Node * > nodes(8);

                        nodes[7] = & node(this->nodeCount() - 2);
                        nodes[6] = & node(this->nodeCount() - 1);
                        nodes[5] = & node(this->nodeCount() - 1 - x.size());
                        nodes[4] = & node(this->nodeCount() - 2 - x.size());

                        nodes[3] = & node(this->nodeCount() - 2 - x.size() * y.size());
                        nodes[2] = & node(this->nodeCount() - 1 - x.size() * y.size());
                        nodes[1] = & node(this->nodeCount() - 1 - x.size() - x.size() * y.size());
                        nodes[0] = & node(this->nodeCount() - 2 - x.size() - x.size() * y.size());
                        this->createCell(nodes, marker);
                    } //** first row/column/layer
                } //** x loop (i)
                if (markerType == 1) marker = 0;
                if (j > 0 && k > 0 && markerType == 13) marker -= (x.size() - 1);
            } //** y loop (j)
//            if (k > 0 && markerType == 13) marker -= (x.size() - 1) * (y.size() - 1);
            if (k > 0 && markerType == 13) marker += (x.size() - 1);
            if (markerType == 2 || markerType == 12) marker = 0;
        } //** z loop (k)
        this->createNeighbourInfos();

        for (Index i = 0; i < boundaryCount(); i ++){
            if (boundary(i).leftCell() == NULL || boundary(i).rightCell() == NULL){
                boundary(i).setMarker(1);
            }
        }

    } else {
        std::cerr << WHERE_AM_I << "Warning! there are too few positions given: "
            << x.size() << " " << y.size() << " " << z.size() << std::endl;
    }
}

void Mesh::createMeshByBoundaries(const Mesh & mesh, const std::vector < Boundary * > & bounds){
    this->clear();
    this->setDimension(mesh.dim());

    std::map < int, Node* > nodeMap;

    //** Create new nodes
    for (size_t i = 0; i < bounds.size(); i ++){
        MeshEntity * ent = bounds[i];
        for (uint j = 0; j < ent->nodeCount(); j ++){
             if (nodeMap.count(ent->node(j).id()) == 0){
                 nodeMap[ent->node(j).id()] = this->createNode(ent->node(j));
             }
        }
    }

    //! Create new boundaries
    for (size_t i = 0; i < bounds.size(); i ++){
        MeshEntity * ent = bounds[i];
        std::vector < Node * > nodes(ent->nodeCount());
        for (uint j = 0; j < nodes.size(); j ++){
            nodes[j] = nodeMap[ent->node(j).id()];
        }

        createBoundary(nodes, bounds[i]->marker());
    }

}

void Mesh::createMeshByCellIdx(const Mesh & mesh, std::vector < int > & idxList){
    this->clear();
    this->setDimension(mesh.dim());

    std::map < int, Node* > nodeMap;

    //** Create new nodes
    for (size_t i = 0; i < idxList.size(); i ++){
        Cell * cell = &mesh.cell(idxList[i]);
        for (uint j = 0; j < cell->nodeCount(); j ++){
            if (nodeMap.count(cell->node(j).id()) == 0){

                nodeMap[cell->node(j).id()] =
                        this->createNode(cell->node(j).pos(),
                                          cell->node(j).marker());
            }
        }
    }

    //! Create new cells
    for (size_t i = 0; i < idxList.size(); i ++){
        Cell * cell = &mesh.cell(idxList[i]);
        std::vector < Node * > nodes(cell->nodeCount());
        for (uint j = 0; j < nodes.size(); j ++){
            nodes[j] = nodeMap[cell->node(j).id()];
        }
        createCell(nodes, cell->marker());
    }

    //! copy all boundary with marker != 0
    for (uint i = 0, imax = mesh.boundaryCount(); i < imax; i ++){
        Boundary * bound = &mesh.boundary(i);

        if (bound->marker() != 0){
            bool inside = true;
            std::vector < Node * > nodes(bound->nodeCount());

            for (uint j = 0, jmax = bound->nodeCount(); j < jmax; j ++){
                if (nodeMap.find(bound->node(j).id()) != nodeMap.end()) {
                    nodes[j] = nodeMap[bound->node(j).id()];
                } else {
                    inside = false;
                    break;
                }
            }
            if (inside){
                //! check that all nodes have a common cell
                if (findCommonCell(nodes, false)){
                    createBoundary(nodes, bound->marker());
                }
            }
        }
    }
    //! Create all remaining boundarys
    createNeighbourInfos();
}

void Mesh::createMeshByMarker(const Mesh & mesh, int from, int to){
    if (to == -1) to = MAX_INT;
    else if (to == 0) to = from + 1;

    std::vector < int > cellIdx;

    for (uint i = 0; i < mesh.cellCount(); i ++){
        if (mesh.cell(i).marker() >= from && mesh.cell(i).marker() < to){
            cellIdx.push_back(i);
        }
    }
    createMeshByCellIdx(mesh, cellIdx);
}

void Mesh::addExportData(const std::string & name, const RVector & data) {
  //  std::cout << "add export Data: " << name << " " << min(data) << " "  << max(data) << std::endl;
    if (exportDataMap_.count(name)){
        exportDataMap_[name] = data;
    } else {
        exportDataMap_.insert(std::make_pair(name, data));
    }
}

RVector Mesh::exportData(const std::string & name) const {
    if (exportDataMap_.count(name)){
        return exportDataMap_.find(name)->second;
    } else {
        std::cerr << WHERE_AM_I << " Warning!! requested export 'data' vector " << name << " does not exist." << std::endl;
    }
    return RVector(0);
}

void Mesh::clearExportData(){
    exportDataMap_.clear();
}

std::vector < int > Mesh::nodeMarker() const {
    std::vector < int > tmp(nodeCount());
    std::transform(nodeVector_.begin(), nodeVector_.end(), tmp.begin(), std::mem_fun(& Node::marker));
    return tmp;
}

std::vector < int > Mesh::boundaryMarker() const {
    std::vector < int > tmp(boundaryCount());
    std::transform(boundaryVector_.begin(), boundaryVector_.end(), tmp.begin(),
                    std::mem_fun(&Boundary::marker));
    return tmp;
}

std::vector < int > Mesh::cellMarker() const{
    std::vector < int > tmp(cellCount());
    std::transform(cellVector_.begin(), cellVector_.end(), tmp.begin(),
                    std::mem_fun(&Cell::marker));
    return tmp;
}

RVector Mesh::cellAttributes() const{
    #ifdef _MSC_VER
	std::vector< double > tmp(cellCount());
    std::transform(cellVector_.begin(), cellVector_.end(), tmp.begin(),
                    std::mem_fun(&Cell::attribute));
	return tmp;
	#else
	RVector tmp(cellCount());
    std::transform(cellVector_.begin(), cellVector_.end(), tmp.begin(),
                    std::mem_fun(&Cell::attribute));
    return tmp;
	#endif
}

void Mesh::setCellAttributes(const RVector & attr){
    if (attr.size() != (uint)cellCount()){
        throwError(1, WHERE_AM_I + " std::vector attr.size() != cellCount()" + toStr(attr.size()) + " " + toStr(cellCount()));
    }
    for (uint i = 0; i < cellCount(); i ++) cell(i).setAttribute(attr[i]);
}

void Mesh::setCellAttributes(double attr){
    for (uint i = 0; i < cellCount(); i ++) cell(i).setAttribute(attr);
}

void Mesh::mapCellAttributes(const std::map < float, float > & aMap){
    std::map< float, float >::const_iterator itm;

    if (aMap.size() != 0){
        for (uint i = 0, imax = cellCount(); i < imax; i++){
            itm = aMap.find(float(cell(i).marker()));
            if (itm != aMap.end()) cell(i).setAttribute((*itm).second);
        }
    }
}

void Mesh::mapAttributeToParameter(const std::vector< int > & cellMapIndex,
                                    const RVector & attributeMap, double defaultVal){
    DEPRECATED
//     RVector mapModel(attributeMap.size() + 2);
//     mapModel[1] = defaultVal;
//   //mapModel[std::slice(2, (int)attributeMap.size(), 1)] = attributeMap;
//     for (uint i = 0; i < attributeMap.size(); i ++) mapModel[i + 2] = attributeMap[i];
//
//     std::vector< Cell * > emptyList;
//
//     for (uint i = 0, imax = cellCount(); i < imax; i ++){
//         if (cellMapIndex[i] > (int)mapModel.size() -1){
//             std::cerr << WHERE_AM_I << " cellMapIndex[i] > attributeMap " << cellMapIndex[i]
//                     << " " << mapModel.size() << std::endl;
//         }
//
//         cell(i).setAttribute(mapModel[cellMapIndex[i]]);
//
//         if (mapModel[cellMapIndex[i]] < TOLERANCE){
//             emptyList.push_back(&cell(i));
//         }
//     }

    //fillEmptyCells(emptyList);
}

void Mesh::mapBoundaryMarker(const std::map < int, int > & aMap){
    std::map< int, int >::const_iterator itm;
    if (aMap.size() != 0){
        for (uint i = 0, imax = boundaryCount(); i < imax; i++){
            itm = aMap.find(boundary(i).marker());
            if (itm != aMap.end()){
	       boundary(i).setMarker((*itm).second);
            }
        }
    }
}

void Mesh::fillEmptyCells(const std::vector< Cell * > & emptyList, double background){
    if (emptyList.size() == 0) return;

    if (background != -1.0){
        for (size_t i = 0; i < emptyList.size(); i ++) emptyList[i]->setAttribute(background);
        return;
    }

    bool smooth = false;
    createNeighbourInfos();
    if (emptyList.size() > 0){
        //std::cout << "Prolongate " << emptyList.size() << " empty cells." << std::endl;
        std::vector< Cell * > nextVector;
        Cell * cell;

        std::map< Cell*, double > prolongationMap;

        for (size_t i = 0; i < emptyList.size(); i ++){
            cell = emptyList[i];

            uint count = 0;
            double val = 0.0;
            for (uint j = 0; j < cell->neighbourCellCount(); j ++){
                Cell * nCell = cell->neighbourCell(j);
                if (nCell){
                    if (nCell->attribute() > TOLERANCE){
                        val += nCell->attribute();
                        count ++;
                    }
                }
            }
            if (count == 0) {
                nextVector.push_back(cell);
            } else {
                if (smooth){
                    cell->setAttribute(val / (double)count);
                } else {
                    prolongationMap[cell] = val / (double)count;
                }
            }
        }

        if (!smooth){//**apply std::map< uint, val > prolongationMap;
            for (std::map< Cell *, double >::iterator it= prolongationMap.begin();
                it != prolongationMap.end(); it ++){
                    it->first->setAttribute(it->second);
            }
        }
        if (emptyList.size() == nextVector.size()){
            save("fillEmptyCellsFail.bms");
            std::cerr << WHERE_AM_I << " WARNING!! cannot fill emptyList: see fillEmptyCellsFail.bms"<< std::endl;
            std::cerr << "trying to fix"<< std::endl;

            for (size_t i = 0; i < emptyList.size(); i ++) emptyList[i]->setAttribute(mean(this->cellAttributes()));
            nextVector.clear();
        }
        fillEmptyCells(nextVector, background);
    } //** if emptyList.size() > 0
}

Mesh & Mesh::scale(const RVector3 & s){
    std::for_each(nodeVector_.begin(), nodeVector_.end(),
                  boost::bind(& Node::scale, _1, boost::ref(s)));
   //for (uint i = 0; i < nodeVector_.size(); i ++) nodeVector_[i]->scale(s);
    rangesKnown_ = false;
    return *this;
}

Mesh & Mesh::translate(const RVector3 & t){
    std::for_each(nodeVector_.begin(), nodeVector_.end(),
                   boost::bind(& Node::translate, _1, boost::ref(t)));
    //for (uint i = 0; i < nodeVector_.size(); i ++) nodeVector_[i]->pos().translate(t);
    rangesKnown_ = false;
    return *this;
}

Mesh & Mesh::rotate(const RVector3 & r){
    std::for_each(nodeVector_.begin(), nodeVector_.end(),
                   boost::bind(& Node::rotate, _1, boost::ref(r)));
    //for (uint i = 0; i < nodeVector_.size(); i ++) nodeVector_[i]->pos().rotate(r);
    rangesKnown_ = false;
    return *this;
}

void Mesh::relax(){
   THROW_TO_IMPL
   //  int E = 0;

//     for (int T = 6; T >= 3; T--){
//       for (int s = 0; s < Ns; s++){
// 	if (side[s].mark == 0){
// 	  if ((node[side[s].a].mark == 0) &&
// 	       (node[side[s].b].mark == 0) &&
// 	       (node[side[s].c].mark == 0) &&
// 	       (node[side[s].d].mark == 0)) {
// 	    E = node[side[s].c].Nne + node[side[s].d].Nne - node[side[s].a].Nne - node[side[s].b].Nne;

// 	    if (E == T) {
// 	      node[side[s].a].Nne++; node[side[s].b].Nne++;
// 	      node[side[s].c].Nne--; node[side[s].d].Nne--;
// 	      swap_side(s);
// 	    }
// 	  }
// 	}
//       }
//     }
  }

void Mesh::smooth(bool nodeMoving, bool edgeSwapping, uint smoothFunction, uint smoothIteration){
    createNeighbourInfos();

    for (Index j = 0; j < smoothIteration; j++){
//         if (edgeSwapping) {
//             for (uint i = 0; i < boundaryCount(); i++) dynamic_cast< Edge & >(boundary(i)).swap(1);
//         }
        if (nodeMoving) {
            for (Index i = 0; i < nodeCount(); i++) {
                bool forbidMove = (node(i).marker() != 0);

                for (std::set< Boundary * >::iterator it = node(i).boundSet().begin();
                     it != node(i).boundSet().end(); it ++){
                    forbidMove = forbidMove || (*it)->marker() != 0;
                    forbidMove = forbidMove || ((*it)->leftCell() == NULL || (*it)->rightCell() == NULL);
                }
                if (!forbidMove) node(i).smooth(smoothFunction);
            }
        }
    }
}

void Mesh::fillKDTree_() const {
    if (!tree_) tree_ = new KDTreeWrapper();

    if (tree_->size() != nodeCount()){
        if (tree_->size() == 0){
//            for (uint i = 0; i < nodeCount(); i ++) tree_->insert(nodeVector_[i]);
            for_each(nodeVector_.begin(), nodeVector_.end(), boost::bind(&KDTreeWrapper::insert, tree_, _1));

            tree_->tree()->optimize();
        } else {
            throwError(1, WHERE_AM_I + toStr(this) + " kd-tree is only partially filled: this should no happen: nodeCount = " + toStr(nodeCount())
                                      + " tree-size() " + toStr(tree_->size()));
        }
    }
}

} // namespace GIMLI
