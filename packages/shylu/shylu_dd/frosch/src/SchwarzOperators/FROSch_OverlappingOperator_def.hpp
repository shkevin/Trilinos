//@HEADER
// ************************************************************************
//
//               ShyLU: Hybrid preconditioner package
//                 Copyright 2012 Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact Alexander Heinlein (alexander.heinlein@uni-koeln.de)
//
// ************************************************************************
//@HEADER

#ifndef _FROSCH_OVERLAPPINGOPERATOR_DEF_HPP
#define _FROSCH_OVERLAPPINGOPERATOR_DEF_HPP

#include <FROSch_OverlappingOperator_decl.hpp>


namespace FROSch {
    
    using namespace Teuchos;
    using namespace Xpetra;

    template <class SC,class LO,class GO,class NO>
    OverlappingOperator<SC,LO,GO,NO>::OverlappingOperator(ConstXMatrixPtr k,
                                                          ParameterListPtr parameterList) :
    SchwarzOperator<SC,LO,GO,NO> (k,parameterList),
    OverlappingMatrix_ (),
    OverlappingMap_ (),
    Scatter_(),
    SubdomainSolver_ (),
    Multiplicity_(),
    Combine_(),
    LevelID_(this->ParameterList_->get("Level ID",1))
    {
        if (!this->ParameterList_->get("Combine Values in Overlap","Restricted").compare("Averaging")) {
            Combine_ = Averaging;
        } else if (!this->ParameterList_->get("Combine Values in Overlap","Restricted").compare("Full")) {
            Combine_ = Full;
        } else if (!this->ParameterList_->get("Combine Values in Overlap","Restricted").compare("Restricted")) {
            Combine_ = Restricted;
        }
    }

    template <class SC,class LO,class GO,class NO>
    OverlappingOperator<SC,LO,GO,NO>::~OverlappingOperator()
    {
        SubdomainSolver_.reset();
    }

    // Y = alpha * A^mode * X + beta * Y
    template <class SC,class LO,class GO,class NO>
    void OverlappingOperator<SC,LO,GO,NO>::apply(const XMultiVector &x,
                                                 XMultiVector &y,
                                                 bool usePreconditionerOnly,
                                                 ETransp mode,
                                                 SC alpha,
                                                 SC beta) const
    {
        FROSCH_ASSERT(this->IsComputed_,"ERROR: OverlappingOperator has to be computed before calling apply()");

        XMultiVectorPtr xTmp = MultiVectorFactory<SC,LO,GO,NO>::Build(x.getMap(),x.getNumVectors());
        *xTmp = x;

        if (!usePreconditionerOnly && mode == NO_TRANS) {
            this->K_->apply(x,*xTmp,mode,ScalarTraits<SC>::one(),ScalarTraits<SC>::zero());
        }

        XMultiVectorPtr xOverlap;
        XMultiVectorPtr xOverlapTmp; // AH 11/28/2018: For Epetra, xOverlap will only have a view to the values of xOverlapTmp. Therefore, xOverlapTmp should not be deleted before xOverlap is used.
        XMultiVectorPtr yOverlap = MultiVectorFactory<SC,LO,GO,NO>::Build(OverlappingMatrix_->getDomainMap(),x.getNumVectors());

        // AH 11/28/2018: replaceMap does not update the GlobalNumRows. Therefore, we have to create a new MultiVector on the serial Communicator. In Epetra, we can prevent to copy the MultiVector.
        #ifdef HAVE_XPETRA_EPETRA
        if (xTmp->getMap()->lib() == UseEpetra) {
            xOverlapTmp = MultiVectorFactory<SC,LO,GO,NO>::Build(OverlappingMap_,x.getNumVectors());

            xOverlapTmp->doImport(*xTmp,*Scatter_,INSERT);

            const RCP<const EpetraMultiVectorT<GO,NO> > xEpetraMultiVectorXOverlapTmp = rcp_dynamic_cast<const EpetraMultiVectorT<GO,NO> >(xOverlapTmp);
            RCP<Epetra_MultiVector> epetraMultiVectorXOverlapTmp = xEpetraMultiVectorXOverlapTmp->getEpetra_MultiVector();

            const RCP<const EpetraMapT<GO,NO> >& xEpetraMap = rcp_dynamic_cast<const EpetraMapT<GO,NO> >(OverlappingMatrix_->getRangeMap());
            Epetra_BlockMap epetraMap = xEpetraMap->getEpetra_BlockMap();

            double *A;
            int MyLDA;
            epetraMultiVectorXOverlapTmp->ExtractView(&A,&MyLDA);

            RCP<Epetra_MultiVector> epetraMultiVectorXOverlap(new Epetra_MultiVector(::View,epetraMap,A,MyLDA,x.getNumVectors()));
            xOverlap = RCP<EpetraMultiVectorT<GO,NO> >(new EpetraMultiVectorT<GO,NO>(epetraMultiVectorXOverlap));
        } else
        #endif
        {
            xOverlap = MultiVectorFactory<SC,LO,GO,NO>::Build(OverlappingMap_,x.getNumVectors());

            xOverlap->doImport(*xTmp,*Scatter_,INSERT);

            xOverlap->replaceMap(OverlappingMatrix_->getRangeMap());
        }
        SubdomainSolver_->apply(*xOverlap,*yOverlap,mode,ScalarTraits<SC>::one(),ScalarTraits<SC>::zero());
        yOverlap->replaceMap(OverlappingMap_);

        xTmp->putScalar(ScalarTraits<SC>::zero());
        if (Combine_ == Restricted){
            GO globID = 0;
            LO localID = 0;
            for (UN i=0; i<y.getNumVectors(); i++) {
                for (UN j=0; j<y.getMap()->getNodeNumElements(); j++) {
                    globID = y.getMap()->getGlobalElement(j);
                    localID = yOverlap->getMap()->getLocalElement(globID);
                    xTmp->getDataNonConst(i)[j] = yOverlap->getData(i)[localID];
                }
            }
        } else {
            xTmp->doExport(*yOverlap,*Scatter_,ADD);
        }
        if (Combine_ == Averaging) {
            ConstSCVecPtr scaling = Multiplicity_->getData(0);
            for (UN j=0; j<xTmp->getNumVectors(); j++) {
                SCVecPtr values = xTmp->getDataNonConst(j);
                for (UN i=0; i<values.size(); i++) {
                    values[i] = values[i] / scaling[i];
                }
            }
        }

        if (!usePreconditionerOnly && mode != NO_TRANS) {
            this->K_->apply(*xTmp,*xTmp,mode,ScalarTraits<SC>::one(),ScalarTraits<SC>::zero());
        }
        y.update(alpha,*xTmp,beta);
    }

    template <class SC,class LO,class GO,class NO>
    int OverlappingOperator<SC,LO,GO,NO>::initializeOverlappingOperator()
    {

        Scatter_ = ImportFactory<LO,GO,NO>::Build(this->getDomainMap(),OverlappingMap_);
        if (Combine_ == Averaging) {
            Multiplicity_ = MultiVectorFactory<SC,LO,GO,NO>::Build(this->getRangeMap(),1);
            XMultiVectorPtr multiplicityRepeated;
            multiplicityRepeated = MultiVectorFactory<SC,LO,GO,NO>::Build(OverlappingMap_,1);
            multiplicityRepeated->putScalar(ScalarTraits<SC>::one());
            XExportPtr multiplicityExporter = ExportFactory<LO,GO,NO>::Build(multiplicityRepeated->getMap(),this->getRangeMap());
            Multiplicity_->doExport(*multiplicityRepeated,*multiplicityExporter,ADD);
        }

        return 0; // RETURN VALUE
    }

    template <class SC,class LO,class GO,class NO>
    int OverlappingOperator<SC,LO,GO,NO>::computeOverlappingOperator()
    {

        if (this->IsComputed_) { // already computed once and we want to recycle the information. That is why we reset OverlappingMatrix_ to K_, because K_ has been reset at this point
            OverlappingMatrix_ = this->K_;
        }

        OverlappingMatrix_ = ExtractLocalSubdomainMatrix(OverlappingMatrix_.getConst(),OverlappingMap_);

        SubdomainSolver_.reset(new SubdomainSolver<SC,LO,GO,NO>(OverlappingMatrix_,sublist(this->ParameterList_,"Solver")));
        SubdomainSolver_->initialize();

        int ret = SubdomainSolver_->compute();

        return ret; // RETURN VALUE
    }

}

#endif
