/************************************************************************
 * Copyright(c) 2012, One Unified. All rights reserved.                 *
 * email: info@oneunified.net                                           *
 *                                                                      *
 * This file is provided as is WITHOUT ANY WARRANTY                     *
 *  without even the implied warranty of                                *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                *
 *                                                                      *
 * This software may not be used nor distributed without proper license *
 * agreement.                                                           *
 *                                                                      *
 * See the file LICENSE.txt for redistribution information.             *
 ************************************************************************/

// _148423314X 2017 Data Mining Algorithms in C++: Data Patterns and Algorithms for Modern Applications
//    may have some interesting thoughts on data mining inter-day and intra-day data for relationships

#include "stdafx.h"

#include <algorithm>

#include <OUCommon/TimeSource.h>

#include <TFIQFeed/BuildInstrument.h>

#include <TFTrading/InstrumentManager.h>

#include "MasterPortfolio.h"

MasterPortfolio::MasterPortfolio(
    pProvider_t pExec, pProvider_t pData1, pProvider_t pData2,
    fGatherOptionDefinitions_t fGatherOptionDefinitions,
    fGetTableRowDef_t fGetTableRowDef,
    pPortfolio_t pMasterPortfolio
    )
  : m_dblPortfolioCashToTrade( 100000.0 ), m_dblPortfolioMargin( 0.25 ), m_nSharesTrading( 0 ),
    m_fOptionNamesByUnderlying( std::move( fGatherOptionDefinitions ) ),
    m_fGetTableRowDef( std::move( fGetTableRowDef ) ),
    m_pMasterPortfolio( pMasterPortfolio ),
    m_pExec( pExec ),
    m_pData1( pData1 ),
    m_pData2( pData2 ),
    m_eAllocate( EAllocate::Waiting )
{
  assert( nullptr != m_fOptionNamesByUnderlying );
  assert( nullptr != m_fGetTableRowDef );

  assert( pExec );
  assert( pData1 );

  switch ( pExec->ID() ) {
    case ou::tf::keytypes::EProviderIB:
      m_pIB = boost::dynamic_pointer_cast<ou::tf::IBTWS>( pExec );
      break;
    default:
      assert( 0 ); // need the IB provider, or at least some provider
  }

  switch ( pData1->ID() ) {
    case ou::tf::keytypes::EProviderIQF:
      m_pIQ = boost::dynamic_pointer_cast<ou::tf::IQFeedProvider>( pData1 );
      break;
    default:
      assert( 0 ); // need the iqfeed provider
  }

  std::stringstream ss;
  ss.str( "" );
  ss << ou::TimeSource::Instance().External();
  // will need to make this generic if need some for multiple providers.
  m_sTSDataStreamStarted = ss.str();  // will need to make this generic if need some for multiple providers.

  m_pOptionEngine.reset( new ou::tf::option::Engine( m_libor ) );
  m_pOptionEngine->m_fBuildWatch
    = [this](pInstrument_t pInstrument)->pWatch_t {
        ou::tf::Watch::pWatch_t pWatch( new ou::tf::Watch( pInstrument, m_pData1 ) );
        return pWatch;
      };
  m_pOptionEngine->m_fBuildOption
    = [this](pInstrument_t pInstrument)->pOption_t {
        ou::tf::option::Option::pOption_t pOption( new ou::tf::option::Option( pInstrument, m_pData1 ) );
        return pOption;
      };

  m_libor.SetWatchOn( m_pIQ );

}

MasterPortfolio::~MasterPortfolio(void) {
  m_libor.SetWatchOff();
  if ( m_worker.joinable() )
    m_worker.join();
  m_mapStrategy.clear();
}

void MasterPortfolio::Load( ptime dtLatestEod, bool bAddToList ) {
  if ( !m_mapStrategy.empty() ) {
    std::cout << "Already have symbols." << std::endl;
  }
  else {
    if ( m_worker.joinable() ) m_worker.join(); // finish existing processing
    m_worker = std::thread(
      [this,dtLatestEod,bAddToList](){
/*
        using setInstrumentInfo_t = SymbolSelection::setIIDarvas_t;
        using InstrumentInfo_t = IIDarvas;
        setInstrumentInfo_t setInstrumentInfo;

        SymbolSelection selector(
          dtLatestEod,
          [&setInstrumentInfo](const InstrumentInfo_t& ii) {
            setInstrumentInfo.insert( ii );
          } );

        if ( bAddToList ) {
          std::for_each( setInstrumentInfo.begin(), setInstrumentInfo.end(),
                        [this](const InstrumentInfo_t& ii){
                          AddSymbol( ii.sName, ii.barLast, ii.dblStop );
                        } );
        }
        else {
          std::cout << "Symbol List: " << std::endl;
          std::for_each(
            setInstrumentInfo.begin(), setInstrumentInfo.end(),
            [this]( const setInstrumentInfo_t::value_type& item ) {
              std::cout << item.sName << std::endl;
            } );
        }
*/
        SymbolSelection selector(
          dtLatestEod,
          [this,bAddToList](const IIPivot& iip) {
            if ( bAddToList ) {
              AddSymbol( iip );
            }
            else {
              std::cout << iip.sName << std::endl;
            }
          } );

        std::cout << "Symbol List finished." << std::endl;
    } );
  }
}

void MasterPortfolio::AddSymbol( const IIPivot& iip ) {

  assert( m_mapStrategy.end() == m_mapStrategy.find( iip.sName ) );

  ou::tf::Portfolio::idPortfolio_t idPortfolio( "Basket_" + iip.sName );
  ou::tf::Portfolio::idAccountOwner_t idAccountOwner( "basket" );
  pPortfolio_t pPortfolioStrategy
    = ou::tf::PortfolioManager::Instance().ConstructPortfolio(
        idPortfolio, idAccountOwner, m_pMasterPortfolio->Id(), ou::tf::Portfolio::EPortfolioType::Standard, ou::tf::Currency::Name[ ou::tf::Currency::USD ], "Basket Case"
    );

  namespace ph = std::placeholders;

  pManageStrategy_t pManageStrategy;
  pManageStrategy.reset( new ManageStrategy(
        iip.sName, iip.bar, pPortfolioStrategy,
    // ManageStrategy::fGatherOptionDefinitions_t
        m_fOptionNamesByUnderlying,
    // ManageStrategy::fConstructWatch_t
        [this](const std::string& sIQFeedEquityName, ManageStrategy::fConstructedWatch_t fWatch){

              const trd_t& trd( m_fGetTableRowDef( sIQFeedEquityName ) ); // TODO: check for errors

              bool bNeedContract( false );
              pInstrument_t pEquityInstrument;
              ou::tf::InstrumentManager& im( ou::tf::InstrumentManager::GlobalInstance().Instance() );

              if ( im.Exists( sIQFeedEquityName, pEquityInstrument ) ) {
                // pEquityInstrument has been populated
                assert( 0 != pEquityInstrument->GetContract() );
              }
              else {
                pEquityInstrument = ou::tf::iqfeed::BuildInstrument( sIQFeedEquityName, trd );  // builds equity option, may not build futures option
                bNeedContract = true;
              }

              pWatch_t pWatch = m_pOptionEngine->m_fBuildWatch( pEquityInstrument );

              // maybe BasketTrading.cpp needs to do the construction, to keep the id's proper?
              if ( bNeedContract ) {
                if ( nullptr == m_pIB.get() ) {
                  throw std::runtime_error( "MasterPortfolio::AddSymbol fConstructWatch_t: IB provider unavailable for contract");
                }
                else {
                    m_pIB->RequestContractDetails(
                      sIQFeedEquityName, pEquityInstrument,
                      [this,pWatch,fWatch](const ou::tf::IBTWS::ContractDetails& details, pInstrument_t& pInstrument){
                        // the contract details fill in the contract in the instrument, which can then be passed back to the caller
                        //   as a fully defined, registered instrument
                        assert( 0 != pInstrument->GetContract() );
                        ou::tf::InstrumentManager& im( ou::tf::InstrumentManager::GlobalInstance().Instance() );
                        im.Register( pInstrument );  // is a CallAfter required, or can this run in a thread?
                        fWatch( pWatch );
                      },
                      nullptr  // request complete doesn't need a function
                      );
                }
              }
              else {
                fWatch( pWatch );
              }
        },
    // ManageStrategy::fConstructOption_t
        [this](const std::string& sIQFeedOptionName, const pInstrument_t pUnderlyingInstrument, ManageStrategy::fConstructedOption_t fOption){

              const trd_t& trd( m_fGetTableRowDef( sIQFeedOptionName ) ); // TODO: check for errors

              std::string sGenericOptionName
                = ou::tf::Instrument::BuildGenericOptionName( pUnderlyingInstrument->GetInstrumentName(), trd.eOptionSide, trd.nYear, trd.nMonth, trd.nDay, trd.dblStrike );

              bool bNeedContract( false );
              pInstrument_t pOptionInstrument;
              ou::tf::InstrumentManager& im( ou::tf::InstrumentManager::GlobalInstance().Instance() );

              if ( im.Exists( sGenericOptionName, pOptionInstrument ) ) {
                // pOptionInstrument has been populated
                assert( 0 != pOptionInstrument->GetContract() );
              }
              else {
                pOptionInstrument = ou::tf::iqfeed::BuildInstrument( sGenericOptionName, trd );  // builds equity option, may not build futures option
                bNeedContract = true;
              }

              pOption_t pOption = m_pOptionEngine->m_fBuildOption( pOptionInstrument );

              if ( bNeedContract ) {
                if ( nullptr == m_pIB.get() ) {
                  throw std::runtime_error( "MasterPortfolio::AddSymbol fConstructOption_t: IB provider unavailable for contract");
                }
                else {
                    m_pIB->RequestContractDetails(
                      pUnderlyingInstrument->GetInstrumentName(), pOptionInstrument,
                      [this,pOption,fOption](const ou::tf::IBTWS::ContractDetails& details, pInstrument_t& pInstrument){
                        // the contract details fill in the contract in the instrument, which can then be passed back to the caller
                        //   as a fully defined, registered instrument
                        assert( 0 != pInstrument->GetContract() );
                        ou::tf::InstrumentManager& im( ou::tf::InstrumentManager::GlobalInstance().Instance() );
                        im.Register( pInstrument );  // is a CallAfter required, or can this run in a thread?
                        fOption( pOption );
                      },
                      nullptr  // request complete doesn't need a function
                      );
                }
              }
              else {
                fOption( pOption );
              }
        },
    // ManageStrategy::fConstructPosition_t
        [this,pPortfolioStrategy]( const ou::tf::Portfolio::idPortfolio_t idPortfolio, pWatch_t pWatch)->ManageStrategy::pPosition_t{

              // maybe BasketTrading.cpp needs to do the construction, to keep the id's proper?
              pPosition_t pPosition = ou::tf::PortfolioManager::Instance().ConstructPosition(
                idPortfolio, pWatch->GetInstrument()->GetInstrumentName(), "Basket", "ib01", "iq01", m_pExec, pWatch );

              return pPosition;
          },
    // ManageStrategy::fStartCalc_t
          std::bind( &ou::tf::option::Engine::Add, m_pOptionEngine.get(), ph::_1, ph::_2 ),
    // ManageStrategy::m_fStopCalc
          std::bind( &ou::tf::option::Engine::Remove, m_pOptionEngine.get(), ph::_1, ph::_2 ),
    // ManageStrategy::m_fFirstTrade
          [this](ManageStrategy& ms, const ou::tf::Trade& trade){
            // calculate the starting parameters
            mapStrategy_t::iterator iter = m_mapStrategy.find( ms.GetUnderlying() );
            assert( m_mapStrategy.end() != iter );
            Strategy& strategy( iter->second );
            strategy.priceOpen = trade.Price();
            IIPivot::Pair pair = strategy.iip.Test( trade.Price() );
            std::cout << "FirstTrade " << m_mapPivotProbability.size() << " - " << strategy.iip.sName << ": " << (int)pair.second << "," << pair.first << std::endl;
            switch ( m_eAllocate ) {
              case EAllocate::Waiting:
                //mapPivotProbability_t::iterator iterRanking =
                  m_mapPivotProbability.insert( mapPivotProbability_t::value_type( pair.first, Ranking( strategy.iip.sName, pair.second ) ) );
                if ( 100 < m_mapPivotProbability.size() ) {
                  double dblAmountToTradePerInstrument = /* 3% */ 0.03 * ( m_dblPortfolioCashToTrade / m_dblPortfolioMargin ); // ~ 33 instances
                  std::for_each(
                    m_mapPivotProbability.rbegin(),
                    m_mapPivotProbability.rend(),
                    [this,dblAmountToTradePerInstrument](mapPivotProbability_t::value_type& vt){
                      Ranking& ranking( vt.second );

                      Strategy& strategy( m_mapStrategy.find( ranking.sName )->second );
                      if ( 200 <= strategy.pManageStrategy->CalcShareCount( dblAmountToTradePerInstrument ) ) {
                        strategy.pManageStrategy->SetFundsToTrade( dblAmountToTradePerInstrument );
                        m_nSharesTrading += strategy.pManageStrategy->CalcShareCount( dblAmountToTradePerInstrument );
                        //strategy.pManageStrategy->Start();
                        switch ( ranking.direction ) {
                          case IIPivot::Direction::Up:
                            strategy.pManageStrategy->Start( ManageStrategy::ETradeDirection::Up );
                            break;
                          case IIPivot::Direction::Down:
                            strategy.pManageStrategy->Start( ManageStrategy::ETradeDirection::Down );
                            break;
                          case IIPivot::Direction::Unknown:
                            assert( 0 );
                            break;
                        }
                      }
                    } );
                  std::cout << "#Shares to be traded: " << m_nSharesTrading << std::endl;
                }
                m_eAllocate = EAllocate::Done;
                break;
              case EAllocate::Process:
                break;
              case EAllocate::Done:
                break;
            }

          },
    // ManageStrategy::m_fBar (60 second)
          [this](ManageStrategy& ms, const ou::tf::Bar& bar){
            // calculate sentiment
            m_sentiment.Update( bar );
          }
        )
      );

    Strategy strategy( std::move( iip ), std::move( pManageStrategy ) );
    m_mapStrategy.insert( mapStrategy_t::value_type( strategy.iip.sName, std::move( strategy ) ) ); // lookup needs to come before move
} // AddSymbol

void MasterPortfolio::GetSentiment( size_t& nUp, size_t& nDown ) const {
  m_sentiment.Get( nUp, nDown );
}

void MasterPortfolio::Start() {

  assert( 0 != m_mapStrategy.size() );

  // first pass: to get rough idea of which can be traded given our funding level
  double dblAmountToTradePerInstrument = ( m_dblPortfolioCashToTrade / m_dblPortfolioMargin ) / m_mapStrategy.size();

  unsigned int cntToBeTraded = 0;

  std::for_each( m_mapStrategy.begin(), m_mapStrategy.end(),
                [&cntToBeTraded, dblAmountToTradePerInstrument](mapStrategy_t::value_type& pair){
                  Strategy& strategy( pair.second );
                  if ( 200 <= strategy.pManageStrategy->CalcShareCount( dblAmountToTradePerInstrument ) ) {
                    cntToBeTraded++;
                    //strategy.pManageStrategy->ToBeTraded() = true;
                  }
                  else {
                    //strategy.pManageStrategy->ToBeTraded() = false;
                  }
                } );

  // second pass: start trading with the ones that we can
  dblAmountToTradePerInstrument = ( m_dblPortfolioCashToTrade / m_dblPortfolioMargin ) / cntToBeTraded;

  if ( nullptr != m_pIB.get() ) {
      std::for_each( m_mapStrategy.begin(), m_mapStrategy.end(),
                    [dblAmountToTradePerInstrument, this](mapStrategy_t::value_type& pair){
                      Strategy& strategy( pair.second );
//                      if ( strategy.pManageStrategy->ToBeTraded() ) {

//                        strategy.pManageStrategy->SetFundsToTrade( dblAmountToTradePerInstrument );
//                        m_nSharesTrading += strategy.pManageStrategy->CalcShareCount( dblAmountToTradePerInstrument );

//                        strategy.pManageStrategy->Start();

//                      }
                    } );
  }
  else {
    std::cout << "IB provider not active, can't get IB symbols" << std::endl;
  }

  std::cout << "#Shares to be traded: " << m_nSharesTrading << std::endl;
} // Start

void MasterPortfolio::Stop( void ) {
  std::for_each( m_mapStrategy.begin(), m_mapStrategy.end(),
                [](mapStrategy_t::value_type& pair){
                  Strategy& strategy( pair.second );
//                  if ( strategy.pManageStrategy->ToBeTraded() ) {
//                    strategy.pManageStrategy->Stop();
//                  }
                } );
}

void MasterPortfolio::SaveSeries( const std::string& sPrefix ) {
  std::cout << "Saving ... ";
  std::string sPath( sPrefix + m_sTSDataStreamStarted );
  m_libor.SaveSeries( sPath );
  std::for_each(m_mapStrategy.begin(), m_mapStrategy.end(),
                [&sPath](mapStrategy_t::value_type& pair){
                  Strategy& strategy( pair.second );
                  strategy.pManageStrategy->SaveSeries( sPath );
                } );
  std::cout << "done." << std::endl;
}

