/***************************************************************************
  qgsdistancearea.cpp - Distance and area calculations on the ellipsoid
 ---------------------------------------------------------------------------
  Date                 : September 2005
  Copyright            : (C) 2005 by Martin Dobias
  email                : won.der at centrum.sk
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include <cmath>
#include <QString>
#include <QObject>

#include "qgsdistancearea.h"
#include "qgis.h"
#include "qgspointxy.h"
#include "qgscoordinatetransform.h"
#include "qgscoordinatereferencesystem.h"
#include "qgsgeometry.h"
#include "qgsgeometrycollection.h"
#include "qgslogger.h"
#include "qgsmessagelog.h"
#include "qgsmultisurface.h"
#include "qgswkbptr.h"
#include "qgslinestring.h"
#include "qgspolygon.h"
#include "qgssurface.h"
#include "qgsunittypes.h"
#include "qgsexception.h"
#include "qgsmultilinestring.h"

#include <geodesic.h>

#define DEG2RAD(x)    ((x)*M_PI/180)
#define RAD2DEG(r) (180.0 * (r) / M_PI)
#define POW2(x) ((x)*(x))

QgsDistanceArea::QgsDistanceArea()
{
  // init with default settings
  mSemiMajor = -1.0;
  mSemiMinor = -1.0;
  mInvFlattening = -1.0;
  QgsCoordinateTransformContext context; // this is ok - by default we have a source/dest of WGS84, so no reprojection takes place
  setSourceCrs( QgsCoordinateReferenceSystem::fromSrsId( GEOCRS_ID ), context ); // WGS 84
  setEllipsoid( GEO_NONE );
}

bool QgsDistanceArea::willUseEllipsoid() const
{
  return mEllipsoid != GEO_NONE;
}

void QgsDistanceArea::setSourceCrs( const QgsCoordinateReferenceSystem &srcCRS, const QgsCoordinateTransformContext &context )
{
  mCoordTransform.setContext( context );
  mCoordTransform.setSourceCrs( srcCRS );
}

bool QgsDistanceArea::setEllipsoid( const QString &ellipsoid )
{
  // Shortcut if ellipsoid is none.
  if ( ellipsoid == GEO_NONE )
  {
    mEllipsoid = GEO_NONE;
    return true;
  }

  QgsEllipsoidUtils::EllipsoidParameters params = QgsEllipsoidUtils::ellipsoidParameters( ellipsoid );
  if ( !params.valid )
  {
    return false;
  }
  else
  {
    mEllipsoid = ellipsoid;
    setFromParams( params );
    return true;
  }
}

// Inverse flattening is calculated with invf = a/(a-b)
// Also, b = a-(a/invf)
bool QgsDistanceArea::setEllipsoid( double semiMajor, double semiMinor )
{
  mEllipsoid = QStringLiteral( "PARAMETER:%1:%2" ).arg( qgsDoubleToString( semiMajor ), qgsDoubleToString( semiMinor ) );
  mSemiMajor = semiMajor;
  mSemiMinor = semiMinor;
  mInvFlattening = mSemiMajor / ( mSemiMajor - mSemiMinor );

  computeAreaInit();

  return true;
}

double QgsDistanceArea::measure( const QgsAbstractGeometry *geomV2, MeasureType type ) const
{
  if ( !geomV2 )
  {
    return 0.0;
  }

  int geomDimension = geomV2->dimension();
  if ( geomDimension <= 0 )
  {
    return 0.0;
  }

  MeasureType measureType = type;
  if ( measureType == Default )
  {
    measureType = ( geomDimension == 1 ? Length : Area );
  }

  if ( !willUseEllipsoid() )
  {
    //no transform required
    if ( measureType == Length )
    {
      return geomV2->length();
    }
    else
    {
      return geomV2->area();
    }
  }
  else
  {
    //multigeom is sum of measured parts
    const QgsGeometryCollection *collection = qgsgeometry_cast<const QgsGeometryCollection *>( geomV2 );
    if ( collection )
    {
      double sum = 0;
      for ( int i = 0; i < collection->numGeometries(); ++i )
      {
        sum += measure( collection->geometryN( i ), measureType );
      }
      return sum;
    }

    if ( measureType == Length )
    {
      const QgsCurve *curve = qgsgeometry_cast<const QgsCurve *>( geomV2 );
      if ( !curve )
      {
        return 0.0;
      }

      QgsLineString *lineString = curve->curveToLine();
      double length = measureLine( lineString );
      delete lineString;
      return length;
    }
    else
    {
      const QgsSurface *surface = qgsgeometry_cast<const QgsSurface *>( geomV2 );
      if ( !surface )
        return 0.0;

      QgsPolygon *polygon = surface->surfaceToPolygon();

      double area = 0;
      const QgsCurve *outerRing = polygon->exteriorRing();
      area += measurePolygon( outerRing );

      for ( int i = 0; i < polygon->numInteriorRings(); ++i )
      {
        const QgsCurve *innerRing = polygon->interiorRing( i );
        area -= measurePolygon( innerRing );
      }
      delete polygon;
      return area;
    }
  }
}

double QgsDistanceArea::measureArea( const QgsGeometry &geometry ) const
{
  if ( geometry.isNull() )
    return 0.0;

  const QgsAbstractGeometry *geomV2 = geometry.constGet();
  return measure( geomV2, Area );
}

double QgsDistanceArea::measureLength( const QgsGeometry &geometry ) const
{
  if ( geometry.isNull() )
    return 0.0;

  const QgsAbstractGeometry *geomV2 = geometry.constGet();
  return measure( geomV2, Length );
}

double QgsDistanceArea::measurePerimeter( const QgsGeometry &geometry ) const
{
  if ( geometry.isNull() )
    return 0.0;

  const QgsAbstractGeometry *geomV2 = geometry.constGet();
  if ( !geomV2 || geomV2->dimension() < 2 )
  {
    return 0.0;
  }

  if ( !willUseEllipsoid() )
  {
    return geomV2->perimeter();
  }

  //create list with (single) surfaces
  QVector< const QgsSurface * > surfaces;
  const QgsSurface *surf = qgsgeometry_cast<const QgsSurface *>( geomV2 );
  if ( surf )
  {
    surfaces.append( surf );
  }
  const QgsMultiSurface *multiSurf = qgsgeometry_cast<const QgsMultiSurface *>( geomV2 );
  if ( multiSurf )
  {
    surfaces.reserve( ( surf ? 1 : 0 ) + multiSurf->numGeometries() );
    for ( int i = 0; i  < multiSurf->numGeometries(); ++i )
    {
      surfaces.append( static_cast<const QgsSurface *>( multiSurf->geometryN( i ) ) );
    }
  }

  double length = 0;
  QVector<const QgsSurface *>::const_iterator surfaceIt = surfaces.constBegin();
  for ( ; surfaceIt != surfaces.constEnd(); ++surfaceIt )
  {
    if ( !*surfaceIt )
    {
      continue;
    }

    QgsPolygon *poly = ( *surfaceIt )->surfaceToPolygon();
    const QgsCurve *outerRing = poly->exteriorRing();
    if ( outerRing )
    {
      length += measure( outerRing );
    }
    int nInnerRings = poly->numInteriorRings();
    for ( int i = 0; i < nInnerRings; ++i )
    {
      length += measure( poly->interiorRing( i ) );
    }
    delete poly;
  }
  return length;
}

double QgsDistanceArea::measureLine( const QgsCurve *curve ) const
{
  if ( !curve )
  {
    return 0.0;
  }

  QgsPointSequence linePointsV2;
  QVector<QgsPointXY> linePoints;
  curve->points( linePointsV2 );
  QgsGeometry::convertPointList( linePointsV2, linePoints );
  return measureLine( linePoints );
}

double QgsDistanceArea::measureLine( const QVector<QgsPointXY> &points ) const
{
  if ( points.size() < 2 )
    return 0;

  double total = 0;
  QgsPointXY p1, p2;

  try
  {
    if ( willUseEllipsoid() )
      p1 = mCoordTransform.transform( points[0] );
    else
      p1 = points[0];

    for ( QVector<QgsPointXY>::const_iterator i = points.constBegin(); i != points.constEnd(); ++i )
    {
      if ( willUseEllipsoid() )
      {
        p2 = mCoordTransform.transform( *i );
        total += computeDistanceBearing( p1, p2 );
      }
      else
      {
        p2 = *i;
        total += measureLine( p1, p2 );
      }

      p1 = p2;
    }

    return total;
  }
  catch ( QgsCsException &cse )
  {
    Q_UNUSED( cse );
    QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform a point. Unable to calculate line length." ) );
    return 0.0;
  }

}

double QgsDistanceArea::measureLine( const QgsPointXY &p1, const QgsPointXY &p2 ) const
{
  double result;

  try
  {
    QgsPointXY pp1 = p1, pp2 = p2;

    QgsDebugMsgLevel( QStringLiteral( "Measuring from %1 to %2" ).arg( p1.toString( 4 ), p2.toString( 4 ) ), 3 );
    if ( willUseEllipsoid() )
    {
      QgsDebugMsgLevel( QStringLiteral( "Ellipsoidal calculations is enabled, using ellipsoid %1" ).arg( mEllipsoid ), 4 );
      QgsDebugMsgLevel( QStringLiteral( "From proj4 : %1" ).arg( mCoordTransform.sourceCrs().toProj4() ), 4 );
      QgsDebugMsgLevel( QStringLiteral( "To   proj4 : %1" ).arg( mCoordTransform.destinationCrs().toProj4() ), 4 );
      pp1 = mCoordTransform.transform( p1 );
      pp2 = mCoordTransform.transform( p2 );
      QgsDebugMsgLevel( QStringLiteral( "New points are %1 and %2, calculating..." ).arg( pp1.toString( 4 ), pp2.toString( 4 ) ), 4 );
      result = computeDistanceBearing( pp1, pp2 );
    }
    else
    {
      QgsDebugMsgLevel( QStringLiteral( "Cartesian calculation on canvas coordinates" ), 4 );
      result = p2.distance( p1 );
    }
  }
  catch ( QgsCsException &cse )
  {
    Q_UNUSED( cse );
    QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform a point. Unable to calculate line length." ) );
    result = 0.0;
  }
  QgsDebugMsgLevel( QStringLiteral( "The result was %1" ).arg( result ), 3 );
  return result;
}

double QgsDistanceArea::measureLineProjected( const QgsPointXY &p1, double distance, double azimuth, QgsPointXY *projectedPoint ) const
{
  double result = 0.0;
  QgsPointXY p2;
  if ( mCoordTransform.sourceCrs().isGeographic() && willUseEllipsoid() )
  {
    p2 = computeSpheroidProject( p1, distance, azimuth );
    result = p1.distance( p2 );
  }
  else // Cartesian coordinates
  {
    result = distance; // Avoid rounding errors when using meters [return as sent]
    if ( sourceCrs().mapUnits() != QgsUnitTypes::DistanceMeters )
    {
      distance = ( distance * QgsUnitTypes::fromUnitToUnitFactor( QgsUnitTypes::DistanceMeters, sourceCrs().mapUnits() ) );
      result = p1.distance( p2 );
    }
    p2 = p1.project( distance, azimuth );
  }
  QgsDebugMsgLevel( QStringLiteral( "Converted distance of %1 %2 to %3 distance %4 %5, using azimuth[%6] from point[%7] to point[%8] sourceCrs[%9] mEllipsoid[%10] isGeographic[%11] [%12]" )
                    .arg( QString::number( distance, 'f', 7 ),
                          QgsUnitTypes::toString( QgsUnitTypes::DistanceMeters ),
                          QString::number( result, 'f', 7 ),
                          mCoordTransform.sourceCrs().isGeographic() ? QStringLiteral( "Geographic" ) : QStringLiteral( "Cartesian" ),
                          QgsUnitTypes::toString( sourceCrs().mapUnits() ) )
                    .arg( azimuth )
                    .arg( p1.asWkt(),
                          p2.asWkt(),
                          sourceCrs().description(),
                          mEllipsoid )
                    .arg( sourceCrs().isGeographic() )
                    .arg( QStringLiteral( "SemiMajor[%1] SemiMinor[%2] InvFlattening[%3] " ).arg( QString::number( mSemiMajor, 'f', 7 ), QString::number( mSemiMinor, 'f', 7 ), QString::number( mInvFlattening, 'f', 7 ) ) ), 4 );
  if ( projectedPoint )
  {
    *projectedPoint = QgsPointXY( p2 );
  }
  return result;
}

/*
 *  From original rttopo documentation:
 *  Tested against:
 *   http://mascot.gdbc.gov.bc.ca/mascot/util1b.html
 *  and
 *   http://www.ga.gov.au/nmd/geodesy/datums/vincenty_direct.jsp
 */
QgsPointXY QgsDistanceArea::computeSpheroidProject(
  const QgsPointXY &p1, double distance, double azimuth ) const
{
  // ellipsoid
  double a = mSemiMajor;
  double b = mSemiMinor;
  double f = 1 / mInvFlattening;
  if ( ( ( a < 0 ) && ( b < 0 ) ) ||
       ( ( p1.x() < -180.0 ) || ( p1.x() > 180.0 ) || ( p1.y() < -85.05115 ) || ( p1.y() > 85.05115 ) ) )
  {
    // latitudes outside these bounds cause the calculations to become unstable and can return invalid results
    return QgsPoint( 0, 0 );

  }
  double radians_lat = DEG2RAD( p1.y() );
  double radians_long = DEG2RAD( p1.x() );
  double b2 = POW2( b ); // spheroid_mu2
  double omf = 1 - f;
  double tan_u1 = omf * std::tan( radians_lat );
  double u1 = std::atan( tan_u1 );
  double sigma, last_sigma, delta_sigma, two_sigma_m;
  double sigma1, sin_alpha, alpha, cos_alphasq;
  double u2, A, B;
  double lat2, lambda, lambda2, C, omega;
  int i = 0;
  if ( azimuth < 0.0 )
  {
    azimuth = azimuth + M_PI * 2.0;
  }
  if ( azimuth > ( M_PI * 2.0 ) )
  {
    azimuth = azimuth - M_PI * 2.0;
  }
  sigma1 = std::atan2( tan_u1, std::cos( azimuth ) );
  sin_alpha = std::cos( u1 ) * std::sin( azimuth );
  alpha = std::asin( sin_alpha );
  cos_alphasq = 1.0 - POW2( sin_alpha );
  u2 = POW2( std::cos( alpha ) ) * ( POW2( a ) - b2 ) / b2; // spheroid_mu2
  A = 1.0 + ( u2 / 16384.0 ) * ( 4096.0 + u2 * ( -768.0 + u2 * ( 320.0 - 175.0 * u2 ) ) );
  B = ( u2 / 1024.0 ) * ( 256.0 + u2 * ( -128.0 + u2 * ( 74.0 - 47.0 * u2 ) ) );
  sigma = ( distance / ( b * A ) );
  do
  {
    two_sigma_m = 2.0 * sigma1 + sigma;
    delta_sigma = B * std::sin( sigma ) * ( std::cos( two_sigma_m ) + ( B / 4.0 ) * ( std::cos( sigma ) * ( -1.0 + 2.0 * POW2( std::cos( two_sigma_m ) ) - ( B / 6.0 ) * std::cos( two_sigma_m ) * ( -3.0 + 4.0 * POW2( std::sin( sigma ) ) ) * ( -3.0 + 4.0 * POW2( std::cos( two_sigma_m ) ) ) ) ) );
    last_sigma = sigma;
    sigma = ( distance / ( b * A ) ) + delta_sigma;
    i++;
  }
  while ( i < 999 && std::fabs( ( last_sigma - sigma ) / sigma ) > 1.0e-9 );

  lat2 = std::atan2( ( std::sin( u1 ) * std::cos( sigma ) + std::cos( u1 ) * std::sin( sigma ) *
                       std::cos( azimuth ) ), ( omf * std::sqrt( POW2( sin_alpha ) +
                           POW2( std::sin( u1 ) * std::sin( sigma ) - std::cos( u1 ) * std::cos( sigma ) *
                                 std::cos( azimuth ) ) ) ) );
  lambda = std::atan2( ( std::sin( sigma ) * std::sin( azimuth ) ), ( std::cos( u1 ) * std::cos( sigma ) -
                       std::sin( u1 ) * std::sin( sigma ) * std::cos( azimuth ) ) );
  C = ( f / 16.0 ) * cos_alphasq * ( 4.0 + f * ( 4.0 - 3.0 * cos_alphasq ) );
  omega = lambda - ( 1.0 - C ) * f * sin_alpha * ( sigma + C * std::sin( sigma ) *
          ( std::cos( two_sigma_m ) + C * std::cos( sigma ) * ( -1.0 + 2.0 * POW2( std::cos( two_sigma_m ) ) ) ) );
  lambda2 = radians_long + omega;
  return QgsPointXY( RAD2DEG( lambda2 ), RAD2DEG( lat2 ) );
}

double QgsDistanceArea::latitudeGeodesicCrossesAntimeridian( const QgsPointXY &pp1, const QgsPointXY &pp2, double &fractionAlongLine ) const
{
  QgsPointXY p1 = pp1;
  QgsPointXY p2 = pp2;
  if ( p1.x() < -120 )
    p1.setX( p1.x() + 360 );
  if ( p2.x() < -120 )
    p2.setX( p2.x() + 360 );

  // we need p2.x() > 180 and p1.x() < 180
  double p1x = p1.x() < 180 ? p1.x() : p2.x();
  double p1y = p1.x() < 180 ? p1.y() : p2.y();
  double p2x = p1.x() < 180 ? p2.x() : p1.x();
  double p2y = p1.x() < 180 ? p2.y() : p1.y();
  // lat/lon are our candidate intersection position - we want this to get as close to 180 as possible
  // the first candidate is p2
  double lat = p2y;
  double lon = p2x;

  if ( mEllipsoid == GEO_NONE )
  {
    fractionAlongLine = ( 180 - p1x ) / ( p2x - p1x );
    if ( p1.x() >= 180 )
      fractionAlongLine = 1 - fractionAlongLine;
    return p1y + ( 180 - p1x ) / ( p2x - p1x ) * ( p2y - p1y );
  }

  geod_geodesic geod;
  geod_init( &geod, mSemiMajor, 1 / mInvFlattening );

  geod_geodesicline line;
  geod_inverseline( &line, &geod, p1y, p1x, p2y, p2x, GEOD_ALL );

  const double totalDist = line.s13;
  double intersectionDist = line.s13;

  int iterations = 0;
  double t = 0;
  // iterate until our intersection candidate is within ~1 mm of the antimeridian (or too many iterations happened)
  while ( std::fabs( lon - 180.0 ) > 0.00000001 && iterations < 100 )
  {
    if ( iterations > 0 && std::fabs( p2x - p1x ) > 5 )
    {
      // if we have too large a range of longitudes, we use a binary search to narrow the window -- this ensures we will converge
      if ( lon < 180 )
      {
        p1x = lon;
        p1y = lat;
      }
      else
      {
        p2x = lon;
        p2y = lat;
      }
      QgsDebugMsgLevel( QStringLiteral( "Narrowed window to %1, %2 - %3, %4" ).arg( p1x ).arg( p1y ).arg( p2x ).arg( p2y ), 4 );

      geod_inverseline( &line, &geod, p1y, p1x, p2y, p2x, GEOD_ALL );
      intersectionDist = line.s13 * 0.5;
    }
    else
    {
      // we have a sufficiently narrow window -- use Newton's method
      // adjust intersection distance by fraction of how close the previous candidate was to 180 degrees longitude -
      // this helps us close in to the correct longitude quickly
      intersectionDist *= ( 180.0 - p1x ) / ( lon - p1x );
    }

    // now work out the point on the geodesic this far from p1 - this becomes our new candidate for crossing the antimeridian

    geod_position( &line, intersectionDist, &lat, &lon, &t );
    // we don't want to wrap longitudes > 180 around)
    if ( lon < 0 )
      lon += 360;

    iterations++;
    QgsDebugMsgLevel( QStringLiteral( "After %1 iterations lon is %2, lat is %3, dist from p1: %4" ).arg( iterations ).arg( lon ).arg( lat ).arg( intersectionDist ), 4 );
  }

  fractionAlongLine = intersectionDist / totalDist;
  if ( p1.x() >= 180 )
    fractionAlongLine = 1 - fractionAlongLine;

  // either converged on 180 longitude or hit too many iterations
  return lat;
}

QgsGeometry QgsDistanceArea::splitGeometryAtAntimeridian( const QgsGeometry &geometry ) const
{
  if ( QgsWkbTypes::geometryType( geometry.wkbType() ) != QgsWkbTypes::LineGeometry )
    return geometry;

  QgsGeometry g = geometry;
  // TODO - avoid segmentization of curved geometries (if this is even possible!)
  if ( QgsWkbTypes::isCurvedType( g.wkbType() ) )
    g.convertToStraightSegment();

  std::unique_ptr< QgsMultiLineString > res = qgis::make_unique< QgsMultiLineString >();
  for ( auto part = g.const_parts_begin(); part != g.const_parts_end(); ++part )
  {
    const QgsLineString *line = qgsgeometry_cast< const QgsLineString * >( *part );
    if ( !line )
      continue;
    if ( line->isEmpty() )
    {
      continue;
    }

    std::unique_ptr< QgsLineString > l = qgis::make_unique< QgsLineString >();
    try
    {
      double x = 0;
      double y = 0;
      double z = 0;
      double m = 0;
      QVector< QgsPoint > newPoints;
      newPoints.reserve( line->numPoints() );
      double prevLon = 0;
      double prevLat = 0;
      double lon = 0;
      double lat = 0;
      double prevZ = 0;
      double prevM = 0;
      for ( int i = 0; i < line->numPoints(); i++ )
      {
        QgsPoint p = line->pointN( i );
        x = p.x();
        if ( mCoordTransform.sourceCrs().isGeographic() )
        {
          x = std::fmod( x, 360.0 );
          if ( x > 180 )
            x -= 360;
          p.setX( x );
        }
        y = p.y();
        lon = x;
        lat = y;
        mCoordTransform.transformInPlace( lon, lat, z );

        //test if we crossed the antimeridian in this segment
        if ( i > 0 && ( ( prevLon < -120 && lon > 120 ) || ( prevLon > 120 && lon  < -120 ) ) )
        {
          // we did!
          // when crossing the antimeridian, we need to calculate the latitude
          // at which the geodesic intersects the antimeridian
          double fract = 0;
          double lat180 = latitudeGeodesicCrossesAntimeridian( QgsPointXY( prevLon, prevLat ), QgsPointXY( lon, lat ), fract );
          if ( line->is3D() )
          {
            z = prevZ + ( p.z() - prevZ ) * fract;
          }
          if ( line->isMeasure() )
          {
            m = prevM + ( p.m() - prevM ) * fract;
          }

          QgsPointXY antiMeridianPoint;
          if ( prevLon < -120 )
            antiMeridianPoint = mCoordTransform.transform( QgsPointXY( -180, lat180 ), QgsCoordinateTransform::ReverseTransform );
          else
            antiMeridianPoint = mCoordTransform.transform( QgsPointXY( 180, lat180 ), QgsCoordinateTransform::ReverseTransform );

          QgsPoint newPoint( antiMeridianPoint );
          if ( line->is3D() )
            newPoint.addZValue( z );
          if ( line->isMeasure() )
            newPoint.addMValue( m );

          if ( std::isfinite( newPoint.x() ) && std::isfinite( newPoint.y() ) )
          {
            newPoints << newPoint;
          }
          res->addGeometry( new QgsLineString( newPoints ) );

          newPoints.clear();
          newPoints.reserve( line->numPoints() - i + 1 );

          if ( lon < -120 )
            antiMeridianPoint = mCoordTransform.transform( QgsPointXY( -180, lat180 ), QgsCoordinateTransform::ReverseTransform );
          else
            antiMeridianPoint = mCoordTransform.transform( QgsPointXY( 180, lat180 ), QgsCoordinateTransform::ReverseTransform );

          if ( std::isfinite( antiMeridianPoint.x() ) && std::isfinite( antiMeridianPoint.y() ) )
          {
            // we want to keep the previously calculated z/m value for newPoint, if present. They're the same each
            // of the antimeridian split
            newPoint.setX( antiMeridianPoint.x() );
            newPoint.setY( antiMeridianPoint.y() );
            newPoints << newPoint;
          }
        }
        newPoints << p;

        prevLon = lon;
        prevLat = lat;
        if ( line->is3D() )
          prevZ = p.z();
        if ( line->isMeasure() )
          prevM = p.m();
      }
      res->addGeometry( new QgsLineString( newPoints ) );
    }
    catch ( QgsCsException & )
    {
      QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform linestring. Unable to calculate break point." ) );
      res->addGeometry( line->clone() );
      break;
    }
  }

  return QgsGeometry( std::move( res ) );
}


QVector< QVector<QgsPointXY> > QgsDistanceArea::geodesicLine( const QgsPointXY &p1, const QgsPointXY &p2, const double interval, const bool breakLine ) const
{
  if ( !willUseEllipsoid() )
  {
    return QVector< QVector< QgsPointXY > >() << ( QVector< QgsPointXY >() << p1 << p2 );
  }

  geod_geodesic geod;
  geod_init( &geod, mSemiMajor, 1 / mInvFlattening );

  QgsPointXY pp1, pp2;
  try
  {
    pp1 = mCoordTransform.transform( p1 );
    pp2 = mCoordTransform.transform( p2 );
  }
  catch ( QgsCsException & )
  {
    QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform a point. Unable to calculate geodesic line." ) );
    return QVector< QVector< QgsPointXY > >();
  }

  geod_geodesicline line;
  geod_inverseline( &line, &geod, pp1.y(), pp1.x(), pp2.y(), pp2.x(), GEOD_ALL );
  const double totalDist = line.s13;

  QVector< QVector< QgsPointXY > > res;
  QVector< QgsPointXY > currentPart;
  currentPart << p1;
  double d = interval;
  double prevLon = pp1.x();
  double prevLat = pp1.y();
  bool lastRun = false;
  double t = 0;
  while ( true )
  {
    double lat, lon;
    if ( lastRun )
    {
      lat = pp2.y();
      lon = pp2.x();
      if ( lon > 180 )
        lon -= 360;
    }
    else
    {
      geod_position( &line, d, &lat, &lon, &t );
    }

    if ( breakLine && ( ( prevLon < -120 && lon > 120 ) || ( prevLon > 120 && lon < -120 ) ) )
    {
      // when breaking the geodesic at the antimeridian, we need to calculate the latitude
      // at which the geodesic intersects the antimeridian, and add points to both line segments at this latitude
      // on the antimeridian.
      double fraction;
      double lat180 = latitudeGeodesicCrossesAntimeridian( QgsPointXY( prevLon, prevLat ), QgsPointXY( lon, lat ), fraction );

      try
      {
        QgsPointXY p;
        if ( prevLon < -120 )
          p = mCoordTransform.transform( QgsPointXY( -180, lat180 ), QgsCoordinateTransform::ReverseTransform );
        else
          p = mCoordTransform.transform( QgsPointXY( 180, lat180 ), QgsCoordinateTransform::ReverseTransform );

        if ( std::isfinite( p.x() ) && std::isfinite( p.y() ) )
          currentPart << p;
      }
      catch ( QgsCsException & )
      {
        QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform a point." ) );
      }

      res << currentPart;
      currentPart.clear();
      try
      {
        QgsPointXY p;
        if ( lon < -120 )
          p = mCoordTransform.transform( QgsPointXY( -180, lat180 ), QgsCoordinateTransform::ReverseTransform );
        else
          p = mCoordTransform.transform( QgsPointXY( 180, lat180 ), QgsCoordinateTransform::ReverseTransform );

        if ( std::isfinite( p.x() ) && std::isfinite( p.y() ) )
          currentPart << p;
      }
      catch ( QgsCsException & )
      {
        QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform a point." ) );
      }

    }

    prevLon = lon;
    prevLat = lat;

    try
    {
      currentPart << mCoordTransform.transform( QgsPointXY( lon, lat ), QgsCoordinateTransform::ReverseTransform );
    }
    catch ( QgsCsException & )
    {
      QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform a point." ) );
    }

    if ( lastRun )
      break;

    d += interval;
    if ( d >= totalDist )
      lastRun = true;
  }
  res << currentPart;
  return res;
}

QgsUnitTypes::DistanceUnit QgsDistanceArea::lengthUnits() const
{
  return willUseEllipsoid() ? QgsUnitTypes::DistanceMeters : mCoordTransform.sourceCrs().mapUnits();
}

QgsUnitTypes::AreaUnit QgsDistanceArea::areaUnits() const
{
  return willUseEllipsoid() ? QgsUnitTypes::AreaSquareMeters :
         QgsUnitTypes::distanceToAreaUnit( mCoordTransform.sourceCrs().mapUnits() );
}

double QgsDistanceArea::measurePolygon( const QgsCurve *curve ) const
{
  if ( !curve )
  {
    return 0.0;
  }

  QgsPointSequence linePointsV2;
  curve->points( linePointsV2 );
  QVector<QgsPointXY> linePoints;
  QgsGeometry::convertPointList( linePointsV2, linePoints );
  return measurePolygon( linePoints );
}


double QgsDistanceArea::measurePolygon( const QVector<QgsPointXY> &points ) const
{
  try
  {
    if ( willUseEllipsoid() )
    {
      QVector<QgsPointXY> pts;
      for ( QVector<QgsPointXY>::const_iterator i = points.constBegin(); i != points.constEnd(); ++i )
      {
        pts.append( mCoordTransform.transform( *i ) );
      }
      return computePolygonArea( pts );
    }
    else
    {
      return computePolygonArea( points );
    }
  }
  catch ( QgsCsException &cse )
  {
    Q_UNUSED( cse );
    QgsMessageLog::logMessage( QObject::tr( "Caught a coordinate system exception while trying to transform a point. Unable to calculate polygon area." ) );
    return 0.0;
  }
}


double QgsDistanceArea::bearing( const QgsPointXY &p1, const QgsPointXY &p2 ) const
{
  QgsPointXY pp1 = p1, pp2 = p2;
  double bearing;

  if ( willUseEllipsoid() )
  {
    pp1 = mCoordTransform.transform( p1 );
    pp2 = mCoordTransform.transform( p2 );
    computeDistanceBearing( pp1, pp2, &bearing );
  }
  else //compute simple planar azimuth
  {
    double dx = p2.x() - p1.x();
    double dy = p2.y() - p1.y();
    bearing = std::atan2( dx, dy );
  }

  return bearing;
}

QgsPointXY QgsDistanceArea::destination( const QgsPointXY &p, double distance, double bearingDeg ) const
{
  double a = mSemiMajor;
  double b = mSemiMinor;
  double f = 1 / mInvFlattening;

  double lon1 = p.x();
  double lat1 = p.y();

  double s = distance;
  double alpha1 = bearingDeg * M_PI / 180.;
  double sinAlpha1 = sin( alpha1 );
  double cosAlpha1 = cos( alpha1 );

  double tanU1 = ( 1. - f ) * tan( lat1 * M_PI / 180. );
  double cosU1 = 1. / sqrt( ( 1. + tanU1 * tanU1 ) );
  double sinU1 = tanU1 * cosU1;
  double sigma1 = atan2( tanU1, cosAlpha1 );
  double sinAlpha = cosU1 * sinAlpha1;
  double cosSqAlpha = 1. - sinAlpha * sinAlpha;
  double uSq = cosSqAlpha * ( a * a - b * b ) / ( b * b );
  double A = 1. + uSq / 16384. * ( 4096. + uSq * ( -768. + uSq * ( 320. - 175. * uSq ) ) );
  double B = uSq / 1024. * ( 256. + uSq * ( -128. + uSq * ( 74. - 47. * uSq ) ) );

  double sigma = s / ( b * A );
  double sigmaP = 2. * M_PI;
  while ( abs( sigma - sigmaP ) > 1E-12 )
  {
    double cos2SigmaM = cos( 2. * sigma1 + sigma );
    double sinSigma = sin( sigma );
    double cosSigma = cos( sigma );
    double deltaSigma = B * sinSigma * ( cos2SigmaM + 0.25 * B * ( cosSigma * ( -1. + 2. * cos2SigmaM * cos2SigmaM ) -
                                         B / 6. * cos2SigmaM * ( -3. + 4. * sinSigma * sinSigma ) * ( -3. + 4. * cos2SigmaM * cos2SigmaM ) ) );
    sigmaP = sigma;
    sigma = s / ( b * A ) + deltaSigma;
  }

  double sinSigma = sin( sigma );
  double cosSigma = cos( sigma );
  double cos2SigmaM = cos( 2. * sigma1 + sigma );
  double tmp = sinU1 * sinSigma - cosU1 * cosSigma * cosAlpha1;
  double lat2 = atan2( sinU1 * cosSigma + cosU1 * sinSigma * cosAlpha1, ( 1. - f ) * sqrt( sinAlpha * sinAlpha + tmp * tmp ) );
  double lambda = atan2( sinSigma * sinAlpha1, cosU1 * cosSigma - sinU1 * sinSigma * cosAlpha1 );
  double C = f / 16. * cosSqAlpha * ( 4. + f * ( 4. - 3. * cosSqAlpha ) );
  double L = lambda - ( 1. - C ) * f * sinAlpha * ( sigma + C * sinSigma * ( cos2SigmaM + C * cosSigma * ( -1. + 2. * cos2SigmaM * cos2SigmaM ) ) );
  return QgsPointXY( lon1 + L * 180. / M_PI, lat2 * 180. / M_PI );
}


///////////////////////////////////////////////////////////
// distance calculation

double QgsDistanceArea::computeDistanceBearing(
  const QgsPointXY &p1, const QgsPointXY &p2,
  double *course1, double *course2 ) const
{
  if ( qgsDoubleNear( p1.x(), p2.x() ) && qgsDoubleNear( p1.y(), p2.y() ) )
    return 0;

  // ellipsoid
  double a = mSemiMajor;
  double b = mSemiMinor;
  double f = 1 / mInvFlattening;

  double p1_lat = DEG2RAD( p1.y() ), p1_lon = DEG2RAD( p1.x() );
  double p2_lat = DEG2RAD( p2.y() ), p2_lon = DEG2RAD( p2.x() );

  double L = p2_lon - p1_lon;
  double U1 = std::atan( ( 1 - f ) * std::tan( p1_lat ) );
  double U2 = std::atan( ( 1 - f ) * std::tan( p2_lat ) );
  double sinU1 = std::sin( U1 ), cosU1 = std::cos( U1 );
  double sinU2 = std::sin( U2 ), cosU2 = std::cos( U2 );
  double lambda = L;
  double lambdaP = 2 * M_PI;

  double sinLambda = 0;
  double cosLambda = 0;
  double sinSigma = 0;
  double cosSigma = 0;
  double sigma = 0;
  double alpha = 0;
  double cosSqAlpha = 0;
  double cos2SigmaM = 0;
  double C = 0;
  double tu1 = 0;
  double tu2 = 0;

  int iterLimit = 20;
  while ( std::fabs( lambda - lambdaP ) > 1e-12 && --iterLimit > 0 )
  {
    sinLambda = std::sin( lambda );
    cosLambda = std::cos( lambda );
    tu1 = ( cosU2 * sinLambda );
    tu2 = ( cosU1 * sinU2 - sinU1 * cosU2 * cosLambda );
    sinSigma = std::sqrt( tu1 * tu1 + tu2 * tu2 );
    cosSigma = sinU1 * sinU2 + cosU1 * cosU2 * cosLambda;
    sigma = std::atan2( sinSigma, cosSigma );
    alpha = std::asin( cosU1 * cosU2 * sinLambda / sinSigma );
    cosSqAlpha = std::cos( alpha ) * std::cos( alpha );
    cos2SigmaM = cosSigma - 2 * sinU1 * sinU2 / cosSqAlpha;
    C = f / 16 * cosSqAlpha * ( 4 + f * ( 4 - 3 * cosSqAlpha ) );
    lambdaP = lambda;
    lambda = L + ( 1 - C ) * f * std::sin( alpha ) *
             ( sigma + C * sinSigma * ( cos2SigmaM + C * cosSigma * ( -1 + 2 * cos2SigmaM * cos2SigmaM ) ) );
  }

  if ( iterLimit == 0 )
    return -1;  // formula failed to converge

  double uSq = cosSqAlpha * ( a * a - b * b ) / ( b * b );
  double A = 1 + uSq / 16384 * ( 4096 + uSq * ( -768 + uSq * ( 320 - 175 * uSq ) ) );
  double B = uSq / 1024 * ( 256 + uSq * ( -128 + uSq * ( 74 - 47 * uSq ) ) );
  double deltaSigma = B * sinSigma * ( cos2SigmaM + B / 4 * ( cosSigma * ( -1 + 2 * cos2SigmaM * cos2SigmaM ) -
                                       B / 6 * cos2SigmaM * ( -3 + 4 * sinSigma * sinSigma ) * ( -3 + 4 * cos2SigmaM * cos2SigmaM ) ) );
  double s = b * A * ( sigma - deltaSigma );

  if ( course1 )
  {
    *course1 = std::atan2( tu1, tu2 );
  }
  if ( course2 )
  {
    // PI is added to return azimuth from P2 to P1
    *course2 = std::atan2( cosU1 * sinLambda, -sinU1 * cosU2 + cosU1 * sinU2 * cosLambda ) + M_PI;
  }

  return s;
}

///////////////////////////////////////////////////////////
// stuff for measuring areas - copied from GRASS
// don't know how does it work, but it's working .)
// see G_begin_ellipsoid_polygon_area() in area_poly1.c

double QgsDistanceArea::getQ( double x ) const
{
  double sinx, sinx2;

  sinx = std::sin( x );
  sinx2 = sinx * sinx;

  return sinx * ( 1 + sinx2 * ( m_QA + sinx2 * ( m_QB + sinx2 * m_QC ) ) );
}


double QgsDistanceArea::getQbar( double x ) const
{
  double cosx, cosx2;

  cosx = std::cos( x );
  cosx2 = cosx * cosx;

  return cosx * ( m_QbarA + cosx2 * ( m_QbarB + cosx2 * ( m_QbarC + cosx2 * m_QbarD ) ) );
}


void QgsDistanceArea::computeAreaInit()
{
  //don't try to perform calculations if no ellipsoid
  if ( mEllipsoid == GEO_NONE )
  {
    return;
  }

  double a2 = ( mSemiMajor * mSemiMajor );
  double e2 = 1 - ( ( mSemiMinor * mSemiMinor ) / a2 );
  double e4, e6;

  m_TwoPI = M_PI + M_PI;

  e4 = e2 * e2;
  e6 = e4 * e2;

  m_AE = a2 * ( 1 - e2 );

  m_QA = ( 2.0 / 3.0 ) * e2;
  m_QB = ( 3.0 / 5.0 ) * e4;
  m_QC = ( 4.0 / 7.0 ) * e6;

  m_QbarA = -1.0 - ( 2.0 / 3.0 ) * e2 - ( 3.0 / 5.0 ) * e4  - ( 4.0 / 7.0 ) * e6;
  m_QbarB = ( 2.0 / 9.0 ) * e2 + ( 2.0 / 5.0 ) * e4  + ( 4.0 / 7.0 ) * e6;
  m_QbarC = - ( 3.0 / 25.0 ) * e4 - ( 12.0 / 35.0 ) * e6;
  m_QbarD = ( 4.0 / 49.0 ) * e6;

  m_Qp = getQ( M_PI_2 );
  m_E  = 4 * M_PI * m_Qp * m_AE;
  if ( m_E < 0.0 )
    m_E = -m_E;
}

void QgsDistanceArea::setFromParams( const QgsEllipsoidUtils::EllipsoidParameters &params )
{
  if ( params.useCustomParameters )
  {
    setEllipsoid( params.semiMajor, params.semiMinor );
  }
  else
  {
    mSemiMajor = params.semiMajor;
    mSemiMinor = params.semiMinor;
    mInvFlattening = params.inverseFlattening;
    mCoordTransform.setDestinationCrs( params.crs );
    // precalculate some values for area calculations
    computeAreaInit();
  }
}

double QgsDistanceArea::computePolygonArea( const QVector<QgsPointXY> &points ) const
{
  if ( points.isEmpty() )
  {
    return 0;
  }

  // IMPORTANT
  // don't change anything here without reporting the changes to upstream (GRASS)
  // let's all be good opensource citizens and share the improvements!

  double x1, y1, x2, y2, dx, dy;
  double Qbar1, Qbar2;
  double area;

  /* GRASS comment: threshold for dy, should be between 1e-4 and 1e-7
   * See relevant discussion at https://trac.osgeo.org/grass/ticket/3369
  */
  const double thresh = 1e-6;

  QgsDebugMsgLevel( "Ellipsoid: " + mEllipsoid, 3 );
  if ( !willUseEllipsoid() )
  {
    return computePolygonFlatArea( points );
  }
  int n = points.size();
  x2 = DEG2RAD( points[n - 1].x() );
  y2 = DEG2RAD( points[n - 1].y() );
  Qbar2 = getQbar( y2 );

  area = 0.0;

  for ( int i = 0; i < n; i++ )
  {
    x1 = x2;
    y1 = y2;
    Qbar1 = Qbar2;

    x2 = DEG2RAD( points[i].x() );
    y2 = DEG2RAD( points[i].y() );
    Qbar2 = getQbar( y2 );

    if ( x1 > x2 )
      while ( x1 - x2 > M_PI )
        x2 += m_TwoPI;
    else if ( x2 > x1 )
      while ( x2 - x1 > M_PI )
        x1 += m_TwoPI;

    dx = x2 - x1;
    dy = y2 - y1;
    if ( std::fabs( dy ) > thresh )
    {
      /* account for different latitudes y1, y2 */
      area += dx * ( m_Qp - ( Qbar2 - Qbar1 ) / dy );
    }
    else
    {
      /* latitudes y1, y2 are (nearly) identical */

      /* if y2 becomes similar to y1, i.e. y2 -> y1
       * Qbar2 - Qbar1 -> 0 and dy -> 0
       * (Qbar2 - Qbar1) / dy -> ?
       * (Qbar2 - Qbar1) / dy should approach Q((y1 + y2) / 2)
       * Metz 2017
       */
      area += dx * ( m_Qp - getQ( ( y1 + y2 ) / 2.0 ) );

      /* original:
       * area += dx * getQ( y2 ) - ( dx / dy ) * ( Qbar2 - Qbar1 );
       */
    }
  }
  if ( ( area *= m_AE ) < 0.0 )
    area = -area;

  /* kludge - if polygon circles the south pole the area will be
  * computed as if it cirlced the north pole. The correction is
  * the difference between total surface area of the earth and
  * the "north pole" area.
  */
  if ( area > m_E )
    area = m_E;
  if ( area > m_E / 2 )
    area = m_E - area;

  return area;
}

double QgsDistanceArea::computePolygonFlatArea( const QVector<QgsPointXY> &points ) const
{
  // Normal plane area calculations.
  double area = 0.0;
  int i, size;

  size = points.size();

  // QgsDebugMsg("New area calc, nr of points: " + QString::number(size));
  for ( i = 0; i < size; i++ )
  {
    // QgsDebugMsg("Area from point: " + (points[i]).toString(2));
    // Using '% size', so that we always end with the starting point
    // and thus close the polygon.
    area = area + points[i].x() * points[( i + 1 ) % size].y() - points[( i + 1 ) % size].x() * points[i].y();
  }
  // QgsDebugMsg("Area from point: " + (points[i % size]).toString(2));
  area = area / 2.0;
  return std::fabs( area ); // All areas are positive!
}

QString QgsDistanceArea::formatDistance( double distance, int decimals, QgsUnitTypes::DistanceUnit unit, bool keepBaseUnit )
{
  return QgsUnitTypes::formatDistance( distance, decimals, unit, keepBaseUnit );
}

QString QgsDistanceArea::formatArea( double area, int decimals, QgsUnitTypes::AreaUnit unit, bool keepBaseUnit )
{
  return QgsUnitTypes::formatArea( area, decimals, unit, keepBaseUnit );
}

double QgsDistanceArea::convertLengthMeasurement( double length, QgsUnitTypes::DistanceUnit toUnits ) const
{
  // get the conversion factor between the specified units
  QgsUnitTypes::DistanceUnit measureUnits = lengthUnits();
  double factorUnits = QgsUnitTypes::fromUnitToUnitFactor( measureUnits, toUnits );

  double result = length * factorUnits;
  QgsDebugMsgLevel( QStringLiteral( "Converted length of %1 %2 to %3 %4" ).arg( length )
                    .arg( QgsUnitTypes::toString( measureUnits ) )
                    .arg( result )
                    .arg( QgsUnitTypes::toString( toUnits ) ), 3 );
  return result;
}

double QgsDistanceArea::convertAreaMeasurement( double area, QgsUnitTypes::AreaUnit toUnits ) const
{
  // get the conversion factor between the specified units
  QgsUnitTypes::AreaUnit measureUnits = areaUnits();
  double factorUnits = QgsUnitTypes::fromUnitToUnitFactor( measureUnits, toUnits );

  double result = area * factorUnits;
  QgsDebugMsgLevel( QStringLiteral( "Converted area of %1 %2 to %3 %4" ).arg( area )
                    .arg( QgsUnitTypes::toString( measureUnits ) )
                    .arg( result )
                    .arg( QgsUnitTypes::toString( toUnits ) ), 3 );
  return result;
}
