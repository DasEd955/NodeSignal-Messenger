/* ===================================================================================
client.h -- Declares the public interface for the client component
=================================================================================== */

#ifndef NS_CLIENT_H
#define NS_CLIENT_H

/* int ns_client_run -- Runs the NodeSignal client application

    -- int argc: The number of CLI arguments passed to the program
    -- char **argv: The array of CLI argument strings
    
    -- Used as the main public entry point for starting the client application 
    -- Initializes the networking layer, creates the GTK application, and runs the client event loop
    -- Returns the client application's exit status 
    */
int ns_client_run(int argc, char **argv);

#endif
