[SIMULATED, nr_td_sim] first = RNTI 1 of an acquisition, later = RNTIs 2-4, cold = 1-2, steady = 3-4; seconds of decided RNTIs.

| cell | arm | fb | oracle | tdd | rho | rx | acq | first_mean | first_median | later_mean | later_median | cold_median | steady_median | wrong | wrong_pins | undecidable | truth_cb0_elim | cb0_per_grant | cb0_inadmissible | fail_opens | cb0_bound |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| cuda_cb0 | elim | prior | 0 | none | 0.9 | 4 | 500 | 9.39 | 3.67 | 4.4 | 2 | 3.65 | 1.27 | 0 | 0 | 0 | 0 | 587 | 0 | 0 | 0.002 |
| cuda_fallback200 | elim | prior | 0 | none | 0.9 | 4 | 500 | 48 | 4.66 | 18.6 | 2.67 | 4.69 | 1.63 | 0 | 0 | 0 | 0 | 680 | 0 | 0 | 0.002 |
| cuda_fallback200_1rx | elim | prior | 0 | none | 0.9 | 1 | 500 | 148 | 7.3 | 60.8 | 4.34 | 7.46 | 2.92 | 0 | 0 | 0 | 0 | 700 | 0 | 0 | 0.002 |
