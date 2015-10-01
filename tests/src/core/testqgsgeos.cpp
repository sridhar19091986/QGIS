/***************************************************************************
     TestQgsGeos.cpp
     --------------------------------------
    Date                 : 03 Sept 2014
    Copyright            : (C) 2014 by Marco Hugentobler
    Email                : marco@sourcepole.ch
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsapplication.h"
#include "geos_c.h"
#include "geosextra/geos_c_extra.h"


#include <QtTest/QtTest>
#include <QObject>

class TestQgsGeos: public QObject
{
    Q_OBJECT

  private slots:

    void initTestCase();

    void lineIntersection_data();
    void lineIntersection();
};

void TestQgsGeos::initTestCase()
{
  initGEOS( 0, 0 );
}

void TestQgsGeos::lineIntersection_data()
{
  QTest::addColumn<QString>( "wkt_inputA" );
  QTest::addColumn<QString>( "wkt_inputB" );
  QTest::addColumn<QString>( "precision" );
  QTest::addColumn<QString>( "wkt_result" );

  QTest::newRow( "floating" ) << "LINESTRING(0 -10, 2 10)"
  << "LINESTRING(2 -10, 0 10)"
  << "double"
  << "POINT (1.0000000000000000 0.0000000000000000)";
  QTest::newRow( "fixed" ) << "LINESTRING(0 -10, 2 10)"
  << "LINESTRING(2 -10, 0 10)"
  << "0.5"
  << "POINT (2.0000000000000000 0.0000000000000000)";
}

void TestQgsGeos::lineIntersection()
{
  QFETCH( QString, wkt_inputA );
  QFETCH( QString, wkt_inputB );
  QFETCH( QString, precision );
  QFETCH( QString, wkt_result );

  GEOSGeometry* geomA = GEOSGeomFromWKT( wkt_inputA.toLocal8Bit().data() );
  GEOSGeometry* geomB = GEOSGeomFromWKT( wkt_inputB.toLocal8Bit().data() );

  GEOSPrecisionModel* model;
  if ( precision == "double" )
  {
    model = GEOSPrecisionModel_create( GEOS_PRECISION_FLOATING );
  }
  else if ( precision == "single" )
  {
    model = GEOSPrecisionModel_create( GEOS_PRECISION_FLOATING_SINGLE );
  }
  else
  {
    bool ok = false;
    double scale = precision.toDouble( &ok );
    Q_ASSERT( ok );
    model = GEOSPrecisionModel_createFixed( scale );
  }

  GEOSGeometryPrecisionReducer* reducer = GEOSGeometryPrecisionReducer_create( model );

  GEOSGeometry* geomAr = GEOSGeometryPrecisionReducer_reduce( reducer, geomA );
  GEOSGeometry* geomBr = GEOSGeometryPrecisionReducer_reduce( reducer, geomB );

  GEOSGeometry* inter = GEOSIntersection( geomAr, geomBr );

  char* resultwkt = GEOSGeomToWKT( inter );

  QCOMPARE( QString( resultwkt ), wkt_result );

  free( resultwkt );
  GEOSGeom_destroy( geomAr );
  GEOSGeom_destroy( geomBr );
  GEOSGeom_destroy( geomA );
  GEOSGeom_destroy( geomB );
  GEOSGeometryPrecisionReducer_destroy( reducer );
  GEOSPrecisionModel_destroy( model );
}


QTEST_MAIN( TestQgsGeos )
#include "testqgsgeos.moc"
