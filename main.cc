#include <QColor>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextStream>
//#include <QTime>

#include <iostream>

#ifndef Q_OS_LINUX
#error "This is ONLY for Linux."
#endif

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if QT_VERSION < 0x040701
#ifndef QT_SHARED
Q_IMPORT_PLUGIN(qmng)
#endif
#endif

#define SECTORSIZE 512
#define PNGSIZE   1024

struct BadBlocks_t
{
    QSet<qint64>        bads;
    QMap<qint64,qint64> spans;
};

//
static
qint64 GetDeviceSize( QString const& path )
{
    char const *p = path.toLocal8Bit().constData();

    int fd = open( p, O_RDONLY );
    qint64 file_size_in_bytes = 0;
    ioctl( fd, BLKGETSIZE64, &file_size_in_bytes );
    close( fd );
    return file_size_in_bytes;
}

//
static
void WritePng( QString const &path, BadBlocks_t const &badBlocks,
    qint64 devsize, qint64 donesize )
{
    // translate bytes to sectors.
    qint64 const devsec  = devsize / SECTORSIZE;
    qint64 const donesec = donesize / SECTORSIZE;
    double const ratio = static_cast<double>( devsec ) / ( PNGSIZE * PNGSIZE );

    QString pngfile( path );
    pngfile.replace( QChar( '/' ), "_" );
    pngfile.prepend( "badblocks" );

    QImage png( (int)PNGSIZE, (int)PNGSIZE, QImage::Format_Indexed8 );

    if( png.isNull() )
    {
        std::cout << "Not enough memory to allocate image!" << std::endl;
        return;
    }

    png.setColorCount( 4 );
    png.setColor( 0, qRgb( 0, 0, 0 ) ); // black
    png.setColor( 1, qRgb( 255, 255, 255 ) ); // white
    png.setColor( 2, qRgb( 255, 0, 0 ) ); // red
    png.setColor( 3, qRgb( 100, 100, 100 ) ); // gray

    // fill with black color
    png.fill( 0 );

    // Now fill checked space with white color.
    qint64 const offset = donesec / ratio;
    for( qint64 i = 0; i < offset; ++i )
    {
        qint64 const x = i % PNGSIZE;
        qint64 const y = i / PNGSIZE;
        png.setPixel( (int)x, (int)y, 1 );
    }

    // mark bad spans if any.
    for( QMap<qint64,qint64>::const_iterator i = badBlocks.spans.constBegin();
        i != badBlocks.spans.constEnd(); ++i )
    {
        qint64 const start = i.key() / ratio;
        qint64 const stop  = i.value() / ratio;
        for( qint64 offset = start; offset < stop; ++offset )
        {
            qint64 const x = offset % PNGSIZE;
            qint64 const y = offset / PNGSIZE;
            png.setPixel( (int)x, (int)y, 3 );
        }
    }

    // and mark bad sectors.
    for( QSet<qint64>::const_iterator i = badBlocks.bads.constBegin();
        i != badBlocks.bads.constEnd(); ++i )
    {
        qint64 const offset = (*i) / ratio;
        qint64 const x = offset % PNGSIZE;
        qint64 const y = offset / PNGSIZE;
        png.setPixel( (int)x, (int)y, 2 );
    }

    std::cout << "Writing " << qPrintable( pngfile + ".png" ) << " ..." << std::endl;
    png.save( pngfile + ".png", "PNG" );
    QFile f( pngfile + ".txt" );
    if( f.open( QIODevice::WriteOnly | QIODevice::Text ) )
    {
        QTextStream out( &f );
        for( QSet<qint64>::const_iterator i = badBlocks.bads.constBegin();
            i != badBlocks.bads.constEnd(); ++i )
        {
            out << *i << Qt::endl;
        }
    }
    f.close();
    QFile f1( pngfile + ".span" );
    if( f1.open( QIODevice::WriteOnly | QIODevice::Text ) )
    {
        QTextStream out( &f1 );
        for( QMap<qint64,qint64>::const_iterator i = badBlocks.spans.constBegin();
            i != badBlocks.spans.constEnd(); ++i )
        {
            out << i.key() << ' ' << i.value() << Qt::endl;
        }
    }
    f1.close();
    std::cout << "Written." << std::endl;
}

//
// Returns new block on the device to skip to.
static
qint64 RegisterBadBlock( BadBlocks_t &badBlocks, qint64 minSpan, qint64 badBlock )
{
    badBlocks.bads.insert( badBlock );
    if( minSpan == 0 )
    {
        return badBlock + 1;
    }

    // we have a span. Define its lower and higher boundaries.
    qint64 const offset = ( badBlock * SECTORSIZE ) / minSpan;
    qint64 const lowBlock = ( offset * minSpan ) / SECTORSIZE;
    qint64 const uppBlock = lowBlock + minSpan / SECTORSIZE;

    // Now check if the highest span has uppBlock == our lowBlock. If it is,
    // extend the span, otherwise add new one.
    if( !badBlocks.spans.isEmpty() )
    {
        QMap<qint64, qint64>::iterator i = badBlocks.spans.end();
        --i;
        if( i.value() == lowBlock )
        {
            i.value() = uppBlock;
        }
        else
        {
            badBlocks.spans[lowBlock] = uppBlock;
        }
    }
    else // yeah yeah - ugly duplicated code
    {
        badBlocks.spans[lowBlock] = uppBlock;
    }

    // continue with next span's start.
    return uppBlock;
}

//
static
void GetBadBlocks( QString const &path, BadBlocks_t &badBlocks,
    qint64 devsize, qint64 minSpan )
{
    QFile dev( path );
    if( !dev.open( QIODevice::ReadOnly) )
    {
        return;
    }

    qint64 curBlock = 0;
    QElapsedTimer t;
    t.start();
    QDateTime const dt = QDateTime::currentDateTime();
    int counter = 0;
    qint64 const devBlocks = devsize / SECTORSIZE;

    while( curBlock < devBlocks )
    {
        char buf[SECTORSIZE];
        qint64 const r = dev.read( buf, qint64( SECTORSIZE ) );
        if( r == SECTORSIZE )
        {
            ++curBlock;
        }
        else // error
        {
            std::cout << curBlock << std::endl;
            curBlock = RegisterBadBlock( badBlocks, minSpan, curBlock );
            if( !dev.seek( curBlock * SECTORSIZE ) )
            {
                return;
            }
        }
        // Print statistics once a minute
        if( t.elapsed() >= 60000 )
        {
            t.restart();
            int seconds = dt.secsTo( QDateTime::currentDateTime() );
            if( seconds == 0 )
            {
                seconds = 1;
            }
            qint64 const speed = ( curBlock * SECTORSIZE ) / seconds;
            std::cout << "Current sector: " << curBlock << " (" <<
                ( ( curBlock * 100 ) / devBlocks ) << "%), speed=" <<
                speed << " bps" << std::endl;
            // write PNG every 10 minutes
            if( ++counter >= 10 )
            {
                counter = 0;
                WritePng( path, badBlocks, devsize, curBlock * SECTORSIZE );
            }
        }
    }
}

//
static
bool ParseCommandLine( QStringList const &args, QString &device, qint64 &minSpan )
{
    for( int i = 1; i < args.size(); ++i )
    {
        if( args[i] == "-s" )
        {
            ++i;
            bool ok = false;
            minSpan = ( qint64 ) args[i].toLongLong( &ok );
            if( !ok )
            {
                return false;
            }
            minSpan *= 1024*1024;
        }
        else
        {
            device = args[i];
        }
    }

    return !device.isEmpty() && minSpan >= 0;
}

//
int main( int argc, char *argv[] )
{
    QCoreApplication app( argc,argv );

    QString device;
    qint64 minSpan = 0;
    if( !ParseCommandLine( app.arguments(), device, minSpan ) )
    {
        std::cout << "Usage:\n" << qPrintable( app.arguments()[0] ) <<
            " [-s span] device" << std::endl;
        std::cout << "   -s span: set minimum contiguous span that must be "
            "free from defect.\n"
            "            If any sector within it is bad, the whole span "
            "is marked bad.\n"
            "            If span is zero or not supplied, then each sector "
            "is checked.\n" << std::endl;
        return 1;
    }

    // 1. define device size
    qint64 devsize = GetDeviceSize( device );
    std::cout << "Device size=" << devsize << " bytes" << std::endl;

    // 2. read device and create a list of bad blocks
    BadBlocks_t badBlocks;
    GetBadBlocks( device, badBlocks, devsize, minSpan );

    // 3. output PNG with bad blocks marked.
    WritePng( device, badBlocks, devsize, devsize );

    return 0;
}
