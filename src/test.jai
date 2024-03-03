#import "qoi";

#load "benchmark.jai";

main :: ()
{
    readOk, data, desc := QOIRead( "data/testcard.qoi" );

    tester: RepetitionTester;
    NewTestWave( *tester, data.count );

    while BeginTestCycle( *tester )
    {
        BeginTime( *tester );
        readOk, data, desc = QOIRead( "data/testcard.qoi" );
        EndTime( *tester );
        CountBytes( *tester, data.count );
    }
}
