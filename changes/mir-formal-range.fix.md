A range over a container reached through a reference parameter no longer fails MIR verification: its cursor and staged element carry their own loans below the parameter's entry loan.
