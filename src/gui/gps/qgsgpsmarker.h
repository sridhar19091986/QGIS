/***************************************************************************
    qgsgpsmarker.h  - canvas item which shows a gps marker
    ---------------------
    begin                : 18 December 2009
    copyright            : (C) 2009 Tim Sutton
    email                : tim at linfiniti com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#ifndef QGSGPSMARKER_H
#define QGSGPSMARKER_H

#include "qgsmapcanvasitem.h"
#include "qgscoordinatereferencesystem.h"
#include "qgspointxy.h"
#include <QSvgRenderer>

class QPainter;

/**
 * \ingroup gui
 * A class for marking the position of a gps pointer.
 */
class GUI_EXPORT QgsGpsMarker : public QgsMapCanvasItem
{
  public:
    explicit QgsGpsMarker( QgsMapCanvas *mapCanvas );

    void setCenter( const QgsPointXY &point );
    void setDirection( double direction );

    void paint( QPainter *p ) override;

    QRectF boundingRect() const override;

    void updatePosition() override;

    void setSize( int size );

  protected:

    //! coordinates of the point in the center
    QgsPointXY mCenter;
    //! Size of the marker - e.g. 8 will draw it as 8x8
    int mSize;
    double mDirection = 0;

  private:
    QgsCoordinateReferenceSystem mWgs84CRS;
    QSvgRenderer mSvg;

};

#endif
